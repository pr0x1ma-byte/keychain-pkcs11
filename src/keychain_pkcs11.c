/*
 * Our main driver for the keychain_pkcs11 module
 *
 * Some explanation here, since this gets confusing fast.
 *
 * We maintain an array of SLOTS.  Slots are identified by integers,
 * starting at zero.
 *
 * Each slot can have a single TOKEN associated with it.  A slot can also
 * NOT have a token.
 *
 * Each token has a series of OBJECTS, each identified by a positive integer
 * OBJECT HANDLE (object handles cannot be zero).  Objects can be
 * things like certificates, public keys, private keys. and NSS trust
 * objects.
 *
 * To access objects on a token you open a SESSION, identified by a positive
 * integer SESSION HANDLE (session handles cannot be zero).
 *
 * The object handle namespace is per-token - object 5 on the same token
 * always identifies the same object, even across different sessions.
 *
 * The session handle namespace is global; different sessions can refer
 * to different tokens.  But as specified above, if two sessions are opened
 * to the same token, the object handle namespace is shared; object 5,
 * for example, would refer to the same object.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <dispatch/dispatch.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>

#include "mypkcs11.h"
#include "keychain_pkcs11.h"
#include "tokenwatcher.h"
#include "localauth.h"
#include "certutil.h"
#include "ccglue.h"
#include "debug.h"
#include "tables.h"
#include "config.h"

/* We currently support 2.40 of Cryptoki */

#define CK_MAJOR_VERSION 2
#define CK_MINOR_VERSION 40

/* Our "special" slot numbers we use */
#define CERTIFICATE_SLOT	254

/*
 * Return CKR_SLOT_ID_INVALID if we aren't using a valid slot or we aren't
 * given CERTIFICATE_SLOT.
 *
 * Note: must be called with slot_mutex locked.
 */

#define CHECKSLOT(slot, present) \
do { \
	if (slot != CERTIFICATE_SLOT && slot >= slot_count) { \
		os_log_debug(logsys, "Slot %lu is invalid, returning " \
			     "CKR_SLOT_ID_INVALID", slot); \
		UNLOCK_MUTEX(slot_mutex); \
		return CKR_SLOT_ID_INVALID; \
	} \
	if (slot == CERTIFICATE_SLOT && ! cert_slot_enabled) { \
		os_log_debug(logsys, "Requested cert slot (%lu) but is " \
			     "disabled, returning CKR_SLOT_ID_INVALID", slot); \
		UNLOCK_MUTEX(slot_mutex); \
		return CKR_SLOT_ID_INVALID; \
	} \
	if (present) { \
		if (slot == CERTIFICATE_SLOT) { \
			if (atomic_load(&cert_list_status) != initialized) { \
				os_log_debug(logsys, "Requested certificate " \
					     "slot, but certificate list " \
					     "not initialized yet, returning" \
					     " CKR_TOKEN_NOT_PRESENT"); \
				UNLOCK_MUTEX(slot_mutex); \
				return CKR_TOKEN_NOT_PRESENT; \
			} \
		} else { \
			if (slot_list[slot] == NULL) { \
				os_log_debug(logsys, "Requested token slot " \
					     "but no token present, " \
					     "returning " \
					     "CKR_TOKEN_NOT_PRESENT"); \
				UNLOCK_MUTEX(slot_mutex); \
				return CKR_TOKEN_NOT_PRESENT; \
			} \
		} \
	} \
} while (0)

/*
 * Handling PKCS11 locking.  If we can use native locking with pthreads
 * (CKF_OS_LOCKING_OK) then we do that.  Otherwise we use the API-suppled
 * mutex calls.
 */

typedef union {
	pthread_mutex_t pt;
	void * ck;
} kc_mutex;

static int use_mutex = 0;
static CK_RV (*createmutex)(CK_VOID_PTR_PTR) = NULL;
static CK_RV (*destroymutex)(CK_VOID_PTR) = NULL;
static CK_RV (*lockmutex)(CK_VOID_PTR) = NULL;
static CK_RV (*unlockmutex)(CK_VOID_PTR) = NULL;

#define CREATE_MUTEX(mutex) \
do { \
	int rc; \
	if (use_mutex) { \
		if (createmutex) { \
			rc = (*createmutex)(&mutex.ck); \
		} else { \
			rc = pthread_mutex_init(&mutex.pt, NULL); \
		} \
		if (rc) { \
			os_log_debug(logsys, "create_mutex returned %d", rc); \
		} \
	} \
} while (0)

#define DESTROY_MUTEX(mutex) \
do { \
	int rc; \
	if (use_mutex) { \
		if (destroymutex) { \
			rc = (*destroymutex)(&mutex.ck); \
		} else { \
			rc = pthread_mutex_destroy(&mutex.pt); \
		} \
		if (rc) { \
			os_log_debug(logsys, "destroy_mutex returned %d", rc); \
		} \
	} \
} while (0)

#define LOCK_MUTEX(mutex) \
do { \
	int rc; \
	if (use_mutex) { \
		if (lockmutex) { \
			rc = (*lockmutex)(&mutex.ck); \
		} else { \
			rc = pthread_mutex_lock(&mutex.pt); \
		} \
		if (rc) { \
			os_log_debug(logsys, "lock_mutex returned %d", rc); \
		} \
	} \
} while (0)

#define UNLOCK_MUTEX(mutex) \
do { \
	int rc; \
	if (use_mutex) { \
		if (unlockmutex) { \
			rc = (*unlockmutex)(&mutex.ck); \
		} else { \
			rc = pthread_mutex_unlock(&mutex.pt); \
		} \
		if (rc) { \
			os_log_debug(logsys, "unlock_mutex returned %d", rc); \
		} \
	} \
} while (0)

static kc_mutex sess_mutex;

/*
 * Information about a slot
 *
 * We used to glom everything into one virtual "slot", but that started
 * not working very well when we started to have multiple devices on one
 * machine.  So here's the new way.
 *
 * We segregate things by token identifier; when new tokens are detected
 * we try to find an empty slot for them; if we don't have an empty slot
 * then we create a new one.
 *
 * We always keep at least one slot around when there are no tokens as
 * an "empty" slot.
 *
 * As a note: the "object" namespace is per-token, NOT per-session.  So
 * we maintain a separate list of objects for each token
 */

static kc_mutex slot_mutex;

struct slot_entry {
	CFStringRef		tokenid;	/* Slot Token identifier */
	struct id_info ** 	id_list;	/* Array of slot identities */
	unsigned int		id_count;	/* Array count */
	unsigned int		id_size;	/* Array size */
	struct obj_info *	obj_list;	/* Object list */
	unsigned int		obj_count;	/* Object count */
	unsigned int		obj_size;	/* Object array size */
	bool			logged_in;	/* Are we logged into card? */
	char *			label;		/* Slot label */
	void *			lacontext; 	/* LocalAuth context */
	kc_mutex		entry_mutex;	/* Lock for slot entry */
	unsigned int		refcount;	/* Slot reference count */
};

/* These should get filled in at library start-up time */
static struct slot_entry **slot_list = NULL;
static unsigned int slot_count = 0;
static void slot_entry_free(struct slot_entry *, bool);

/*
 * Our list of identities that is stored on our smartcard
 */

struct id_info {
	SecIdentityRef		ident;		/* Identity reference */
	SecCertificateRef	cert;		/* Identity certificate */
	SecKeyRef		privkey;	/* Identity private key */
	SecKeyRef		pubkey;		/* Identity public key */
	CFDataRef		pkeyhash;	/* Public key hash */
	CK_KEY_TYPE		keytype;	/* Key type */
	SecAccessControlRef	secaccess;	/* Access control reference */
	char *			label;		/* Printable label for id */
	bool			privcansign;	/* Can privkey sign data? */
	bool			privcandecrypt;	/* Can privkey decrypt? */
	bool			pubcanverify;	/* Can pubkey verify? */
	bool			pubcanencrypt;	/* Can pubkey encrypt? */
	bool			pubcanwrap;	/* Can pubkey wrap? */
};

static bool ask_pin = false;			/* Should we ask for a PIN? */

static int add_identity(struct slot_entry *, CFDictionaryRef);
static SecAccessControlRef getaccesscontrol(CFDictionaryRef);
static unsigned int cflistcount(CFTypeRef);	/* Count of list entries */
static CFDictionaryRef cfgetindex(CFTypeRef, unsigned int);/* Entry in list */
static void id_info_free(struct id_info *);
static void id_list_free(struct id_info **, unsigned int);
static CK_KEY_TYPE convert_keytype(CFNumberRef);
static void token_logout(struct slot_entry *);
static void get_index_bytes(unsigned int, unsigned char **, unsigned int *);
static bool add_dict(CFMutableDictionaryRef *, const void *, const void *);
static const struct mechanism_map *get_mechmap(CK_MECHANISM_TYPE);
static bool mech_param_validate(CK_MECHANISM_PTR, const struct mechanism_map *,
				SecKeyAlgorithm *, SecKeyAlgorithm *,
				SecKeyAlgorithm *, CK_MECHANISM_TYPE *);

/*
 * Our object list and the functions to handle them
 *
 * The object types we support are:
 *
 * Certificates (CKO_CERTFICATE).  We only support X.509 certificates
 * Public keys (CKO_PUBLIC_KEY).
 * Private keys (CKO_PRIVATE_KEY).
 *
 * The general rule is the CKA_ID attribute for any of those should all
 * match for a given identity.  Previously I just made this as a CK_ULONG
 * corresponding to the identity array index but I got complaints; now it
 * calls get_index_bytes and returns the shortest number of bytes that
 * correspond to the object index.
 *
 * Previously I had implemented each object list as part of a session, but
 * really the object space is per-token, so I changed the implementation to
 * be global (since right now the Security framework only supports one token).
 *
 * Since the object list contains pointers into the id list, we are using
 * the id mutex to lock the object list as well.
 */

struct obj_info {
	struct id_info *	id;
	/* unsigned char		id_value[sizeof(CK_ULONG)]; */
	CK_OBJECT_CLASS		class;
	CK_ATTRIBUTE_PTR	attrs;
	unsigned int		attr_count;
	unsigned int		attr_size;
};

#define LOG_DEBUG_OBJECT(obj, se) \
	os_log_debug(logsys, "Object %lu (%s)", obj, \
		     getCKOName(se->obj_list[obj].class));

static void build_id_objects(struct slot_entry *);
static void obj_free(struct obj_info **, unsigned int *, unsigned int *);

#if 0
static struct obj_info *id_obj_list = NULL;	/* Identity object list */
static unsigned int id_obj_count = 0;		/* Identity object list count */
static unsigned int id_obj_size = 0;		/* Size of identity obj_list */
#endif

/*
 * Our session "state" for operations that span multiple functions.
 * Basic states are:
 *
 * NONE		- No function pending
 * X_INIT	- Function (Sign, Encrypt, etc) initialized.
 * X_UPDATE	- We've called the appropriate "update" function
 *		  like C_SignUpdate().
 *
 * In the above list, 'X' is one of E (encrypt), D (decrypt),
 * S (sign), or V (verify).

 * We're only allowed one pending operation per session.  So the way
 * this works is when you call a function like C_SignInit(), you
 * have to be in the NONE state, and you move into INIT state.
 *
 * If you call the non-update function (e.g., C_Sign()) you have
 * to be in INIT state and (assuming successful completion) you move
 * back into NONE (because the operation is complete).
 *
 * If you call the update function (C_SignUpdate()) you have to either
 * be in INIT or UPDATE.  You move to UPDATE.
 *
 * For the finalize function (C_SignFinal()) you have to be in UPDATE,
 * and assuming successful completion you move to NONE.
 *
 * In theory this should exclude FindObject operations when a crypto
 * operation is in progress, but we don't have that technical limitation
 * so we don't enforce that.
 */

enum s_state { NO_PENDING, E_INIT, E_UPDATE, D_INIT, D_UPDATE, S_INIT,
               S_UPDATE, V_INIT, V_UPDATE };

/*
 * Our session information.  Anything that modifies a session will need to
 * lock that particular session.  We keep an array of pointers to sessions
 * available; if we need more then reallocate the array.
 *
 * Note that "sess_mutex" is for locking the overall session array,
 * but each session also has a mutex.  Sigh.  Is this overkill?  I have
 * no idea.
 *
 * SecKeyAlgorithms are currently constant CFStringRef so we shouldn't
 * have to worry about maintaing references to it using CFRetain/CFRelease().
 */

struct session {
	kc_mutex 	mutex;			/* Session mutex */
	CK_SLOT_ID	slot_id;		/* Slot identifier */
	struct slot_entry *token;		/* Token pointer */
	struct obj_info *obj_list;		/* Pointer to object list */
	unsigned int	obj_list_count;		/* Copy of object count */
	unsigned int	obj_search_index;	/* Current search index */
	CK_ATTRIBUTE_PTR search_attrs;		/* Search attributes */
	unsigned int	search_attrs_count;	/* Search attribute count */
	enum s_state	state;			/* Session operation state */
	SecKeyRef	key;			/* Key for in-progress op */
	size_t		outsize;		/* Op output size, 0 unknown */
	SecKeyAlgorithm	alg;			/* Algorithm for in-prog op */
	SecKeyAlgorithm dalg;			/* Algorithm, takes digest */
	CK_MECHANISM_TYPE hash_alg;		/* Hash algorithm */
	md_context	mdc;			/* Message digest context */
};

static struct session **sess_list = NULL;	/* Yes, array of pointers */
static unsigned int sess_list_count = 0;
static unsigned int sess_list_size = 0;
static void sess_free(struct session *);

/*
 * Return CKR_SESSION_HANDLE_INVALID if we don't have a valid session
 * for this handle
 */

#define CHECKSESSION(session, var) \
do { \
	LOCK_MUTEX(sess_mutex); \
	session--; \
	if (session >= sess_list_count || sess_list[session] == NULL) { \
		os_log_debug(logsys, "Session handle %lu is invalid, " \
			     "returning CKR_SESSION_HANDLE_INVALID", session); \
		UNLOCK_MUTEX(sess_mutex); \
		return CKR_SESSION_HANDLE_INVALID; \
	} \
	var = sess_list[session]; \
	UNLOCK_MUTEX(sess_mutex); \
} while (0)

/*
 * Our attribute list used for searching
 */

static bool search_object(struct obj_info *, CK_ATTRIBUTE_PTR, unsigned int);
static CK_ATTRIBUTE_PTR find_attribute(struct obj_info *, CK_ATTRIBUTE_TYPE);
static void dump_attribute(const char *, CK_ATTRIBUTE_PTR);

/*
 * Our certificate list.  We use this to store certificates we've imported
 * from the system Keychains.
 */

struct certinfo {
	SecCertificateRef	cert;
	CFDataRef		pkeyhash;
};

static struct certinfo *cert_list = NULL;
static unsigned int cert_list_size = 0;
static unsigned int cert_list_count = 0;
enum certstate { uninitialized, initializing, initialized };
_Atomic static enum certstate cert_list_status = ATOMIC_VAR_INIT(uninitialized);
static bool cert_slot_enabled = false;

static struct obj_info *cert_obj_list = NULL;	/* Cert object list */
static unsigned int cert_obj_count = 0;		/* Cert object list count */
static unsigned int cert_obj_size = 0;		/* Size of identity obj_list */

/*
 * Various structures/functions we need for Keychain certificate import
 */

static const char *default_cert_search[] = {
	"DoD Root CA",
	NULL,
};

static const char *default_cert_applist[] = {
	"firefox",
	"thunderbird",
	NULL,
};

/*
 * Sigh, I realize I have a cert_list AND a certlist.  Not great naming
 * on my part; I just don't want to change it now
 */

struct certlist {
	CFDictionaryRef		certdict;
	struct certlist		*next;
};

struct certcontext {
	struct certlist		*head;
	struct certlist		*tail;
	const void		*match;
};

static void background_cert_scan(void *);
static void scan_certificates(void);
static void add_certificate(CFDictionaryRef, CFMutableSetRef);
static void cert_list_free(void);
static struct certlist *search_certs(CFMutableSetRef, CFArrayRef, CFDataRef);
static void cn_match(const void *, void *);
static void issuer_match(const void *, void *);
static void add_cert_to_list(CFDictionaryRef, struct certcontext *);
static void free_certlist(struct certlist *);
static void build_cert_objects(void);

/*
 * Various other utility functions we need
 */

static void sprintfpad(unsigned char *, size_t, const char *, ...);
static bool boolfromdict(const char *, CFDictionaryRef, CFTypeRef);
static char *getkeylabel(SecKeyRef);
static char *getstrcopy(CFStringRef);
static bool prefkey_found(const char *, const char *, const char **);
static char **prefkey_arrayget(const char *, const char **);
static void array_free(char **);
#ifdef KEYCHAIN_DEBUG
void dumpdict(const char *, CFDictionaryRef);
#endif /* KEYCHAIN_DEBUG */

/*
 * Stuff required for logging; we're using the MacOS X native os_log
 * facility.  To get logs out of this, see log(1).  Specifically, if you
 * want debugging logs, try:
 *
 * log stream --predicate 'subsystem = "mil.navy.nrl.cmf.pkcs11"' --level debug
 */

static void log_init(void *);
os_log_t logsys;
static dispatch_once_t loginit;

/*
 * Declarations for our list of exported PKCS11 functions that we return
 * using C_GetFunctionList()
 */

static CK_FUNCTION_LIST function_list = {
	{ CK_MAJOR_VERSION, CK_MINOR_VERSION },
	/* This seems strange to me, but I guess it's what everyone else does */
#undef CK_PKCS11_FUNCTION_INFO
#define CK_PKCS11_FUNCTION_INFO(name) name ,
#include "pkcs11f.h"
};
#undef CK_PKCS11_FUNCTION_INFO

/*
 * Some convenience functions we use for things we have to continually do
 */

#define FUNCINIT(func) \
do { \
	dispatch_once_f(&loginit, NULL, log_init); \
	os_log_debug(logsys, #func " called"); \
} while (0)

#define FUNCINITCHK(func) \
	FUNCINIT(func); \
do { \
	if (! module_initialized) { \
		os_log_debug(logsys, #func " returning NOT_INITIALIZED"); \
		return CKR_CRYPTOKI_NOT_INITIALIZED; \
	} \
} while (0)

#define NOTSUPPORTED(name, args) \
CK_RV name args { \
	FUNCINITCHK(name); \
	os_log_debug(logsys, "Function " #name " returning NOT SUPPORTED!"); \
	return CKR_FUNCTION_NOT_SUPPORTED; \
}

#define RET(name, val) \
do { \
	os_log_debug(logsys, #name " returning %s", getCKRName(val)); \
	return val; \
} while (0)

static int module_initialized = 0;

/*
 * Our implementation of C_GetFunctionList(), which just returns a pointer
 * to our function list
 */

CK_RV C_GetFunctionList(CK_FUNCTION_LIST_PTR_PTR pPtr)
{
	FUNCINIT(C_GetFunctionList);

	if (! pPtr) {
		RET(C_GetFunctionList, CKR_ARGUMENTS_BAD);
	}

	*pPtr = &function_list;

	RET(C_GetFunctionList, CKR_OK);
}

/*
 * These are in PKCS11 order, to make searching easier
 */

/*
 * Initialize the library and setup anything we need.
 */

CK_RV C_Initialize(CK_VOID_PTR p)
{
	CK_C_INITIALIZE_ARGS_PTR init = (CK_C_INITIALIZE_ARGS_PTR) p;
	const char *progname;

	FUNCINIT(C_Initialize);

	if (module_initialized) {
		RET(C_Initialized, CKR_CRYPTOKI_ALREADY_INITIALIZED);
	}

	if (init) {
		if (init->pReserved) {
			os_log_debug(logsys, "pReserved set, returning");
			RET(C_Initialized, CKR_ARGUMENTS_BAD);
		}
		if (init->flags & CKF_OS_LOCKING_OK) {
			use_mutex = 1;
			os_log_debug(logsys, "OS_LOCKING_OK set, using "
				     "pthread locking");
		} else if (init->CreateMutex || init->DestroyMutex ||
			   init->LockMutex || init->UnlockMutex) {
			use_mutex = 1;
			createmutex = init->CreateMutex;
			destroymutex = init->DestroyMutex;
			lockmutex = init->LockMutex;
			unlockmutex = init->UnlockMutex;
			os_log_debug(logsys, "Using caller-supplied locking "
				     "functions");
		} else {
			use_mutex = 0;
			os_log_debug(logsys, "Not performing any locking");
		}
	} else {
		os_log_debug(logsys, "init was set to NULL");
	}

	CREATE_MUTEX(slot_mutex);
	CREATE_MUTEX(sess_mutex);

	/*
	 * Allocate the initial slot array and set the count correctly.
	 * We always have a minimum count of "1".
	 */

	slot_list = malloc(sizeof(*slot_list) * 1);
	slot_count = 1;
	slot_list[0] = NULL;

	/*
	 * By default we let the Security framework pop up a dialog box
	 * when the PIN is needed, and we will set
	 * CKF_PROTECTED_AUTHENTICATION_PATH in the token information
	 * structure to indicate that the application should NOT prompt
	 * for a PIN.  But some programs are buggy, so let's make it
	 * configurable.  Check to see if the current program name exists
	 * in the "askPIN" preference in our configuration domain (currently
	 * that is "mil.navy.nrl.cmf.pkcs11").  The program name is whatever
	 * is returned by getprogname().  If that program exists, then we
	 * will allow the PIN to be set via C_Login().
	 */

	progname = getprogname();

	if (! prefkey_found("askPIN", progname, NULL)) {
		os_log_debug(logsys, "Program \"%{public}s\" is NOT set to "
			     "ask for PIN, will let Security ask for the PIN",
			     progname);
		ask_pin = false;
	} else {
		os_log_debug(logsys, "Program \"%{public}s\" IS set to ask "
			     "for a PIN, we will prompt for the PIN",
			     progname);
		ask_pin = true;
	}

	/*
	 * Also check to see if this application will create the default
	 * Keychain certificate slot.
	 */

	if (! prefkey_found("keychainCertSlot", progname,
			    default_cert_applist)) {
		os_log_debug(logsys, "Program \"%{public}s\" has the Keychain "
			     "Certificate slot DISABLED", progname);
		cert_slot_enabled = false;
	} else {
		enum certstate status = uninitialized;
		os_log_debug(logsys, "Program \"%{public}s\" has the Keychain "
			     "Certificate slot ENABLED", progname);
		cert_slot_enabled = true;

		/*
		 * Mark that we have a certificate scan running; if one
		 * is running then don't start another.
		 *
		 * In a perfect world this shouldn't happen, but if an
		 * application called C_Finalize() then C_Initialize then
		 * this could mess things up.
		 *
		 * Since by default we are only doing a certificate scan
		 * for a few applications (like firefox) I decided to
		 * make things as simple as possible; this means that an
		 * application that calls C_Finalize() before a certificate
		 * scan is complete will leak memory.  I thought about all
		 * of the complicated gyrations that needed to be done
		 * to fix this, and since this is really only useful for
		 * Firefox (which is typically long-running) I decided to
		 * not deal with it.  Maybe I will address it later.
		 */

		if (atomic_compare_exchange_strong(&cert_list_status,
						   &status, initializing))
			dispatch_async_f(dispatch_get_global_queue(
					  DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
					 NULL, background_cert_scan);
	}

	start_token_watcher();

	module_initialized = 1;

	RET(C_Initalize, CKR_OK);
}

/*
 * Clean up everything from the library
 */

CK_RV C_Finalize(CK_VOID_PTR p)
{
	int i;

	FUNCINITCHK(C_Finalize);

	if (p) {
		os_log_debug(logsys, "pReserved is non-NULL");
		RET(C_Finalize, CKR_ARGUMENTS_BAD);
	}

	/*
	 * Before anything else happens, stop receiving token watcher
	 * events
	 */

	stop_token_watcher();

	LOCK_MUTEX(slot_mutex);
	LOCK_MUTEX(sess_mutex);

	for (i = 0; i < slot_count; i++)
		if (slot_list[i])
			slot_entry_free(slot_list[i], true);

	free(slot_list);

	UNLOCK_MUTEX(sess_mutex);
	UNLOCK_MUTEX(slot_mutex);

	DESTROY_MUTEX(sess_mutex);
	DESTROY_MUTEX(slot_mutex);

	if (atomic_load(&cert_list_status) == initialized) {
		obj_free(&cert_obj_list, &cert_obj_count, &cert_obj_size);
		cert_list_free();
	}

	use_mutex = 0;
	module_initialized = 0;
	cert_slot_enabled = 0;

	RET(C_Finalize, CKR_OK);
}

CK_RV C_GetInfo(CK_INFO_PTR p)
{
	FUNCINITCHK(C_GetInfo);

	if (! p) {
		RET(C_GetInfo, CKR_ARGUMENTS_BAD);
	}

	p->cryptokiVersion.major = CK_MAJOR_VERSION;
	p->cryptokiVersion.minor = CK_MINOR_VERSION;

	p->flags = 0;

	sprintfpad(p->manufacturerID, sizeof(p->manufacturerID),
		   "U.S. Naval Research Lab");

	sprintfpad(p->libraryDescription, sizeof(p->libraryDescription),
		   "Keychain PKCS#11 Bridge Library");

	p->libraryVersion.major = 1;
	p->libraryVersion.minor = 0;

	RET(C_GetInfo, CKR_OK);
}

/* C_GetFunctionList declared above */

CK_RV C_GetSlotList(CK_BBOOL token_present, CK_SLOT_ID_PTR ret_slot_list,
		    CK_ULONG_PTR slot_num)
{
	CK_RV rv = CKR_OK;
	unsigned int i, count, sindex;

	FUNCINITCHK(C_GetSlotList);

	os_log_debug(logsys, "tokens_present = %{bool}d, slot_list = %p, "
		     "slot_num = %d", token_present, slot_list,
		     (int) *slot_num);

	/*
	 * We used to perform the identity scan here, but that has
	 * changed.
	 *
	 * We now register a TKTokenWatcher event handler which will generate
	 * insertion events for current and new tokens.  So by the time we
	 * get HERE, we should already know about any tokens that we
	 * have.
	 *
	 * If a slot list entry is NULL, then the slot has no token.
	 *
	 * If we are using the Keychain certificate slot, then
	 * it always counts as "present".
	 */

	LOCK_MUTEX(slot_mutex);

	/*
	 * Count up how many tokens we have,  If token_present is true,
	 * then only count tokens that exist.
	 */

	if (token_present) {
		for (i = 0, count = 0; i < slot_count; i++)
			if (slot_list[i])
				count++;
	} else {
		count = slot_count;
	}

	if (cert_slot_enabled)
		count++;

	/*
	 * If we can't store all of the slot IDs, then return
	 * BUFFER_TOO_SMALL (but make sure we return the proper count
	 * in the slot_num pointer).  Also short-circuit this test if
	 * ret_slot_list is NULL (in that case, we return CKR_OK).
	 */

	if (ret_slot_list == NULL)
		goto out;

	if (*slot_num < count) {
		rv = CKR_BUFFER_TOO_SMALL;
		goto out;
	}

	/*
	 * Return the slot identifier (if token_present is true, only
	 * if it has a token), which is just an index into our slot list.
	 */

	for (i = 0, sindex = 0; i < slot_count; i++)
		if (! token_present || slot_list[i] != NULL)
			ret_slot_list[sindex++] = i;

	/*
	 * Add the cert slot if enabled, checking to make sure that we
	 * are at the correct array position.
	 */

	if (cert_slot_enabled) {
		if (sindex != count - 1) {
			os_log_debug(logsys, "Internal error: sindex = %d, "
				     "count = %d", (int) sindex, (int) count);
			rv = CKR_GENERAL_ERROR;
			goto out;
		}
		ret_slot_list[sindex] = CERTIFICATE_SLOT;
	}


out:
	UNLOCK_MUTEX(slot_mutex);
	*slot_num = count;

	RET(C_GetSlotList, rv);
}

/*
 * Return information about a "slot"
 */

CK_RV C_GetSlotInfo(CK_SLOT_ID slot_id, CK_SLOT_INFO_PTR slot_info)
{
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_GetSlotInfo);

	os_log_debug(logsys, "slot_id = %d, slot_info = %p", (int) slot_id,
		     slot_info);

	LOCK_MUTEX(slot_mutex);

	CHECKSLOT(slot_id, false);

	if (! slot_info) {
		rv = CKR_ARGUMENTS_BAD;
		goto out;
	}

	/*
	 * We can't really get any useful information out of the Security
	 * framework in terms of information about the "slot" (the reader).
	 * I don't really think it is useful anyway, so just fill in some
	 * dummy values.  The one valid thing we return is the
	 * CKF_TOKEN_PRESENT flag if we have a token inserted or not.
	 */

	sprintfpad(slot_info->manufacturerID,
		   sizeof(slot_info->manufacturerID), "%s",
		   "U.S. Naval Research Lab");

	/*
	 * We've already checked for CERTIFICATE_SLOT being enabled
	 * in the CHECKSLOT macro.
	 */

	if (slot_id == CERTIFICATE_SLOT) {
		sprintfpad(slot_info->slotDescription,
			   sizeof(slot_info->slotDescription), "%s",
			   "Keychain Certificates");
		slot_info->flags = CKF_REMOVABLE_DEVICE;
		if (atomic_load(&cert_list_status) == initialized)
			slot_info->flags |= CKF_TOKEN_PRESENT;
	} else {
		slot_info->flags = CKF_HW_SLOT | CKF_REMOVABLE_DEVICE;
		if (slot_list[slot_id]) {
			sprintfpad(slot_info->slotDescription,
				   sizeof(slot_info->slotDescription), "%s",
				   slot_list[slot_id]->label);
			slot_info->flags |= CKF_TOKEN_PRESENT;
		} else {
			sprintfpad(slot_info->slotDescription,
				   sizeof(slot_info->slotDescription),
				   "Keychain Bridge Library Virtual Slot #%d",
				   (int) slot_id);
		}
	}

	slot_info->hardwareVersion.major = 1;
	slot_info->hardwareVersion.minor = 0;
	slot_info->firmwareVersion.major = 1;
	slot_info->firmwareVersion.minor = 0;

out:
	UNLOCK_MUTEX(slot_mutex);
	RET(C_GetSlotInfo, rv);
}

/*
 * Return information about a token.  Most of this stuff is fabricated;
 * a lot of it doesn't matter, as it deals with things we don't support.
 */

CK_RV C_GetTokenInfo(CK_SLOT_ID slot_id, CK_TOKEN_INFO_PTR token_info)
{
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_GetTokenInfo);

	os_log_debug(logsys, "slot_id = %d, token_info = %p", (int) slot_id,
		     token_info);

	LOCK_MUTEX(slot_mutex);

	CHECKSLOT(slot_id, true);

	if (! token_info) {
		rv = CKR_ARGUMENTS_BAD;
		goto out;
	}

	/*
	 * We can't do any administrative operations, really, from the
	 * Security framework, so basically make it so the token is
	 * read/only.
	 */
	token_info->flags = CKF_WRITE_PROTECTED |
			    CKF_USER_PIN_INITIALIZED |
			    CKF_TOKEN_INITIALIZED;

	/*
	 * Again, we can only have a slot_id == CERTIFICATE_SLOT if
	 * the CHECKSLOT macro says we have it enabled.
	 */

	if (slot_id == CERTIFICATE_SLOT) {
		sprintfpad(token_info->label, sizeof(token_info->label), "%s",
			   "Keychain Certificates");
	} else {
		/*
		 * Since this is used as label in a number of places to display
		 * to the user, make it something useful.  Pick the first
		 * certificate found (if available) and return the subject
		 * summary as the token label.
		 */

		LOCK_MUTEX(slot_list[slot_id]->entry_mutex);

		CFStringRef summary;
		char *label;

		summary = SecCertificateCopySubjectSummary(slot_list[slot_id]->id_list[0]->cert);

		if (summary) {
			label = getstrcopy(summary);
		} else {
			label = strdup("Unknown Keychain Token");
		}

		sprintfpad(token_info->label, sizeof(token_info->label),
			   "%s", label);

		free(label);
		if (summary)
			CFRelease(summary);

		UNLOCK_MUTEX(slot_list[slot_id]->entry_mutex);

		token_info->flags |= CKF_LOGIN_REQUIRED;

		/* 
		 * If we were set to to NOT ask for a PIN in C_Login (see
		 * the function C_Initialize for more info) then set the flag
		 * CKF_PROTECTED_AUTHENTICATION_PATH.
		 */

		if (ask_pin) {
			os_log_debug(logsys, "We are NOT setting the flag "
				     "CKF_PROTECTED_AUTHENTICATION_PATH");
		} else {
			os_log_debug(logsys, "We ARE setting the flag "
				     "CKF_PROTECTED_AUTHENTICATION_PATH");
			token_info->flags |= CKF_PROTECTED_AUTHENTICATION_PATH;
		}
	}

	sprintfpad(token_info->manufacturerID,
		   sizeof(token_info->manufacturerID), "%s",
		   "Unknown Manufacturer");
	sprintfpad(token_info->model, sizeof(token_info->model), "%s",
		   "Unknown Model");
	sprintfpad(token_info->serialNumber, sizeof(token_info->serialNumber),
		   "%s", "000001");

	token_info->ulMaxSessionCount = CK_EFFECTIVELY_INFINITE;
	token_info->ulSessionCount = CK_UNAVAILABLE_INFORMATION;
	token_info->ulMaxRwSessionCount = 0;
	token_info->ulRwSessionCount = 0;
	token_info->ulMaxPinLen = 255;
	token_info->ulMinPinLen = 1;
	token_info->ulTotalPublicMemory = CK_UNAVAILABLE_INFORMATION;
	token_info->ulFreePublicMemory = CK_UNAVAILABLE_INFORMATION;
	token_info->ulTotalPrivateMemory = CK_UNAVAILABLE_INFORMATION;
	token_info->ulFreePrivateMemory = CK_UNAVAILABLE_INFORMATION;
	token_info->hardwareVersion.major = 1;
	token_info->hardwareVersion.minor = 0;
	token_info->firmwareVersion.major = 1;
	token_info->firmwareVersion.minor = 0;
	sprintfpad(token_info->utcTime, sizeof(token_info->utcTime),
		   "%s", "1970010100000000");

out:
	UNLOCK_MUTEX(slot_mutex);
	RET(C_GetTokenInfo, rv);
}

/*
 * Return our list of mechanisms that we support.
 */

CK_RV C_GetMechanismList(CK_SLOT_ID slot_id, CK_MECHANISM_TYPE_PTR mechlist,
			 CK_ULONG_PTR mechnum)
{
	int i;

	FUNCINITCHK(C_GetMechanismList);

	os_log_debug(logsys, "slot_id = %lu, mechlist = %p, mechnum = %lu",
		     slot_id, mechlist, *mechnum);

	LOCK_MUTEX(slot_mutex);
	CHECKSLOT(slot_id, true);
	UNLOCK_MUTEX(slot_mutex);

	/*
	 * It's hard to know exactly what all mechanisms are supported by
	 * a particular token, but we can probably safely return all of the
	 * RSA ones at least (since those should work with any RSA key)
	 */

	/*
	 * Return the list count (and CKR_OK) if mechlist was NULL
	 */

	if (!mechlist) {
		*mechnum = keychain_mechmap_size;
		RET(C_GetMechanismList, CKR_OK);
	}

	/*
	 * Return our mechanisms (or CKR_BUFFER_TOO_SMALL)
	 */

	if (*mechnum < keychain_mechmap_size) {
		*mechnum = keychain_mechmap_size;
		RET(C_GetMechanismList, CKR_BUFFER_TOO_SMALL);
	}

	*mechnum = keychain_mechmap_size;

	for (i = 0; i < keychain_mechmap_size; i++)
		mechlist[i] = keychain_mechmap[i].cki_mech;

	RET(C_GetMechanismList, CKR_OK);
}

/*
 * Return information on a particular mechanism.
 *
 * It's not clear how important this information is, at least for
 * callers of our library.  Return some stuff that seems reasonable.
 */

CK_RV C_GetMechanismInfo(CK_SLOT_ID slot_id, CK_MECHANISM_TYPE mechtype,
			 CK_MECHANISM_INFO_PTR mechinfo)
{
	int i;

	FUNCINITCHK(C_GetMechanismInfo);

	os_log_debug(logsys, "slot_id = %lu, mechtype = %s, mechinfo = %p",
		     slot_id, getCKMName(mechtype), mechinfo);

	LOCK_MUTEX(slot_mutex);
	CHECKSLOT(slot_id, true);
	UNLOCK_MUTEX(slot_mutex);

	for (i = 0; i < keychain_mechmap_size; i++) {
		if (mechtype == keychain_mechmap[i].cki_mech) {
			mechinfo->ulMinKeySize = keychain_mechmap[i].min_keylen;
			mechinfo->ulMaxKeySize = keychain_mechmap[i].max_keylen;
			mechinfo->flags = keychain_mechmap[i].usage_flags;
			RET(C_GetMechanismInfo, CKR_OK);
		}
	}

	RET(C_GetMechanismInfo, CKR_MECHANISM_INVALID);
}

NOTSUPPORTED(C_InitToken, (CK_SLOT_ID slot_id, CK_UTF8CHAR_PTR pin, CK_ULONG pinlen, CK_UTF8CHAR_PTR label))
NOTSUPPORTED(C_InitPIN, (CK_SESSION_HANDLE session, CK_UTF8CHAR_PTR pin, CK_ULONG pinlen))
NOTSUPPORTED(C_SetPIN, (CK_SESSION_HANDLE session, CK_UTF8CHAR_PTR oldpin, CK_ULONG oldpinlen, CK_UTF8CHAR_PTR newpin, CK_ULONG newpinlen))

/*
 * Open a "session". Right now this is mostly a no-op.
 */

CK_RV C_OpenSession(CK_SLOT_ID slot_id, CK_FLAGS flags,
		    CK_VOID_PTR app_callback, CK_NOTIFY notify_callback,
		    CK_SESSION_HANDLE_PTR session)
{
	struct session *sess;
	int i;

	FUNCINITCHK(C_OpenSession);

	os_log_debug(logsys, "slot_id = %d, flags = %#lx, app_callback = %p, "
		     "notify_callback = %p, session_handle = %p", (int) slot_id,
		     flags, app_callback, notify_callback, session);

	LOCK_MUTEX(slot_mutex);
	CHECKSLOT(slot_id, true);

	if (! (flags & CKF_SERIAL_SESSION)) {
		UNLOCK_MUTEX(slot_mutex);
		RET(C_OpenSession, CKR_SESSION_PARALLEL_NOT_SUPPORTED);
	}

	sess = malloc(sizeof(*sess));
	CREATE_MUTEX(sess->mutex);

	/*
	 * Pick the right object list depending if we are using the
	 * true hardware slot or the certificate hardware slot.
	 */

	if (slot_id == CERTIFICATE_SLOT) {
		if (atomic_load(&cert_list_status) == initialized) {
			sess->obj_list = cert_obj_list;
			sess->obj_list_count = cert_obj_count;
		} else {
			sess->obj_list = NULL;
			sess->obj_list_count = 0;;
		}
		sess->token = NULL;
	} else {
		sess->obj_list = slot_list[slot_id]->obj_list;
		sess->obj_list_count = slot_list[slot_id]->obj_count;
		sess->token = slot_list[slot_id];
		LOCK_MUTEX(sess->token->entry_mutex);
		sess->token->refcount++;
		UNLOCK_MUTEX(sess->token->entry_mutex);
	}

	sess->slot_id = slot_id;
	sess->search_attrs = NULL;
	sess->search_attrs_count = 0;
	sess->state = NO_PENDING;
	sess->key = NULL;
	sess->mdc = NULL;

	LOCK_MUTEX(sess_mutex);

	/*
	 * See if we can find a free slot in our session list
	 */

	for (i = 0; i < sess_list_size; i++) {
		if (sess_list[i] == NULL) {
			sess_list[i] = sess;
			*session = i + 1;
			if (i >= sess_list_count)
				sess_list_count = i + 1;
			goto out;
		}
	}

	/*
	 * Looks like we need to grow the session list
	 */

	sess_list_size += 5;

	sess_list = realloc(sess_list, sess_list_size * sizeof(*sess_list));

	for (i = sess_list_count + 1; i < sess_list_size; i++)
		sess_list[i] = NULL;

	sess_list[sess_list_count] = sess;

	*session = ++sess_list_count;
out:
	UNLOCK_MUTEX(sess_mutex);
	UNLOCK_MUTEX(slot_mutex);

	RET(C_OpenSession, CKR_OK);
}

CK_RV C_CloseSession(CK_SESSION_HANDLE session)
{
	struct session *se;

	FUNCINITCHK(C_CloseSession);

	os_log_debug(logsys, "session = %d", (int) session);

	CHECKSESSION(session, se);

	LOCK_MUTEX(sess_mutex);

	sess_free(se);

	sess_list[session] = NULL;

#if 0
	for (i = 0; i < sess_list_count; i++)
		if (sess_list[i] != NULL)
			goto cont;

cont:
#endif
	UNLOCK_MUTEX(sess_mutex);

	RET(C_CloseSession, CKR_OK);
}

CK_RV C_CloseAllSessions(CK_SLOT_ID slot_id)
{
	int i;

	FUNCINITCHK(C_CloseAllSessions);

	os_log_debug(logsys, "slot_id = %d", (int) slot_id);

	LOCK_MUTEX(slot_mutex);
	CHECKSLOT(slot_id, true);
	UNLOCK_MUTEX(slot_mutex);

	LOCK_MUTEX(sess_mutex);

	/*
	 * Only close sessions assigned to this slot
	 */

	for (i = 0; i < sess_list_count; i++) {
		if (sess_list[i] && sess_list[i]->slot_id == slot_id) {
			os_log_debug(logsys, "Closing session %d", i);
			sess_free(sess_list[i]);
			sess_list[i] = NULL;
		}
	}

	UNLOCK_MUTEX(sess_mutex);

	RET(C_CloseAllSessions, CKR_OK);
}

CK_RV C_GetSessionInfo(CK_SESSION_HANDLE session,
		       CK_SESSION_INFO_PTR session_info)
{
	struct session *se;

	FUNCINITCHK(C_GetSessionInfo);

	os_log_debug(logsys, "session = %d, session_info = %p",
		     (int) session, session_info);

	CHECKSESSION(session, se);

	if (!session_info)
		RET(C_GetSessionInfo, CKR_ARGUMENTS_BAD);

	session_info->slotID = se->slot_id;
	session_info->state = (se->token && se->token->logged_in) ?
						CKS_RO_USER_FUNCTIONS :
						CKS_RO_PUBLIC_SESSION;
	session_info->flags = CKF_SERIAL_SESSION ;
	session_info->ulDeviceError = 0;

	RET(C_GetSessionInfo, CKR_OK);
}

NOTSUPPORTED(C_GetOperationState, (CK_SESSION_HANDLE session, CK_BYTE_PTR opstate, CK_ULONG_PTR opstatelen))
NOTSUPPORTED(C_SetOperationState, (CK_SESSION_HANDLE session, CK_BYTE_PTR opstate, CK_ULONG opstatelen, CK_OBJECT_HANDLE enckey, CK_OBJECT_HANDLE authkey))

/*
 * Login to token.  If we actually get passed a PIN here, feed it into the
 * LAContext methods in localauth.m.
 */

CK_RV C_Login(CK_SESSION_HANDLE session, CK_USER_TYPE usertype,
	      CK_UTF8CHAR_PTR pin, CK_ULONG pinlen)
{
	struct session *se;
	int i;
	CK_RV rv = CKR_OK;
	FUNCINITCHK(C_Login);

	os_log_debug(logsys, "session = %d, user_type = %lu", (int) session,
		     usertype);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	/*
	 * If we don't have a token associated with this slot, then
	 * just pretend we succeed.
	 */

	if (! se->token)
		goto out;

	/*
         * I went back and forth here; I finally decided that if a PIN
         * was passed into this function then we should set it.  We
         * use the same PIN for all private keys; that seems a safe
         * assumption for now
	 */

	LOCK_MUTEX(se->token->entry_mutex);

	if (pin) {
		/*
		 * If we don't have a localauth context, then
		 * we can't do anything; in that case, just
		 * return success.
		 */

		if (! se->token->lacontext) {
			os_log_debug(logsys, "localauth context is NULL, "
				     "cannot set PIN, skipping");
			UNLOCK_MUTEX(se->token->entry_mutex);
			goto out;
		}

		for (i = 0; i < se->token->id_count; i++) {
			enum la_keyusage usage;

			os_log_debug(logsys, "Setting PIN for identity %d, "
				     "slot %d", i, (int) se->slot_id);

			usage = se->token->id_list[i]->privcansign ?
						USAGE_SIGN : USAGE_DECRYPT;

			if ((rv = lacontext_auth(se->token->lacontext, pin,
						 pinlen,
						 se->token->id_list[i]->secaccess,
						 usage)) != CKR_OK) {
				/*
				 * The real error should have been logged
				 * in lacontext_auth().
				 */
				UNLOCK_MUTEX(se->token->entry_mutex);
				goto out;
			}
		}
	} else {
		os_log_debug(logsys, "We are NOT setting the PIN");
	}

	se->token->logged_in = true;
	UNLOCK_MUTEX(se->token->entry_mutex);

out:
	UNLOCK_MUTEX(se->mutex);

	RET(C_Login, rv);
}

/*
 * If we set a null password, then that will remove our existing credentials.
 */

CK_RV C_Logout(CK_SESSION_HANDLE session)
{
	struct session *se;
	FUNCINITCHK(C_Logout);

	os_log_debug(logsys, "session = %d", (int) session);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	if (! se->token)
		goto out;

	LOCK_MUTEX(se->token->entry_mutex);
	token_logout(se->token);
	UNLOCK_MUTEX(se->token->entry_mutex);

out:
	UNLOCK_MUTEX(se->mutex);
	RET(C_Logout, CKR_OK);
}

NOTSUPPORTED(C_CreateObject, (CK_SESSION_HANDLE session, CK_ATTRIBUTE_PTR template, CK_ULONG num_attributes, CK_OBJECT_HANDLE_PTR object))
NOTSUPPORTED(C_CopyObject, (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object, CK_ATTRIBUTE_PTR template, CK_ULONG num_attributes, CK_OBJECT_HANDLE_PTR new_object))
NOTSUPPORTED(C_DestroyObject, (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object))
NOTSUPPORTED(C_GetObjectSize, (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object, CK_ULONG_PTR size))

/*
 * Return the value of an attribute for an object
 */

CK_RV C_GetAttributeValue(CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object,
			  CK_ATTRIBUTE_PTR template, CK_ULONG count)
{
	struct session *se;
	CK_RV rv = CKR_OK;
	int i;
	CK_ATTRIBUTE_PTR attr;

	FUNCINITCHK(C_GetAttributeValue);

	os_log_debug(logsys, "session = %d, object = %d, template = %p, "
		     "count = %d", (int) session, (int) object, template,
		     (int) count);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	object--;

	if (object >= se->obj_list_count) {
		UNLOCK_MUTEX(se->mutex);
		RET(C_GetAttributeValue, CKR_OBJECT_HANDLE_INVALID);
	}

	LOG_DEBUG_OBJECT(object, se);

	for (i = 0; i < count; i++) {
		os_log_debug(logsys, "Retrieving attribute: %s",
			     getCKAName(template[i].type));
		if ((attr = find_attribute(&se->obj_list[object],
					   template[i].type))) {
			if (! template[i].pValue) {
				template[i].ulValueLen = attr->ulValueLen;
				os_log_debug(logsys, "pValue was NULL, just "
					     "returning length (%lu)",
					     attr->ulValueLen);
			} else {
				if (template[i].ulValueLen < attr->ulValueLen) {
					os_log_debug(logsys, "Attribute: "
						     "buffer too small "
						     "(%lu, %lu)",
						     template[i].ulValueLen,
						     attr->ulValueLen);
					template[i].ulValueLen =
							attr->ulValueLen;
					rv = CKR_BUFFER_TOO_SMALL;
				} else {
					memcpy(template[i].pValue, attr->pValue,
					       attr->ulValueLen);
					os_log_debug(logsys, "Copied over "
						     "attribute (%lu, %lu)",
						     template[i].ulValueLen,
						     attr->ulValueLen);
					template[i].ulValueLen =
							attr->ulValueLen;
				}
			}
		} else {
			os_log_debug(logsys, "Attribute not found");
			template[i].ulValueLen = CK_UNAVAILABLE_INFORMATION;
			rv = CKR_ATTRIBUTE_TYPE_INVALID;
		}
	}

	UNLOCK_MUTEX(se->mutex);

	RET(C_GetAttributeValue, rv);
}

NOTSUPPORTED(C_SetAttributeValue, (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object, CK_ATTRIBUTE_PTR template, CK_ULONG count))

CK_RV C_FindObjectsInit(CK_SESSION_HANDLE session, CK_ATTRIBUTE_PTR template,
			CK_ULONG count)
{
	struct session *se;
	int i;

	FUNCINITCHK(C_FindObjectsInit);

	os_log_debug(logsys, "session = %d, template = %p, count = %lu",
		     (int) session, template, count);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	se->obj_search_index = 0;

	/*
	 * Copy all of our attributes to search against later
	 */

	se->search_attrs = count ? malloc(sizeof(CK_ATTRIBUTE) * count) : NULL;
	se->search_attrs_count = count;

	for (i = 0; i < count; i++) {
		se->search_attrs[i].type = template[i].type;
		se->search_attrs[i].ulValueLen = template[i].ulValueLen;
		if (se->search_attrs[i].ulValueLen ==
					CK_UNAVAILABLE_INFORMATION) {
			se->search_attrs[i].pValue = NULL;
		} else {
			se->search_attrs[i].pValue =
					malloc(se->search_attrs[i].ulValueLen);
			memcpy(se->search_attrs[i].pValue, template[i].pValue,
			       se->search_attrs[i].ulValueLen);
		}
		dump_attribute("Search template", &se->search_attrs[i]);
	}

	UNLOCK_MUTEX(se->mutex);

	RET(C_FindObjectsInit, CKR_OK);
}

/*
 * Return object identifiers that match our search template.
 */

CK_RV C_FindObjects(CK_SESSION_HANDLE session, CK_OBJECT_HANDLE_PTR object,
		    CK_ULONG maxcount, CK_ULONG_PTR count)
{
	struct session *se;
	unsigned int rc = 0;

	FUNCINITCHK(C_FindObjects);

	os_log_debug(logsys, "session = %d, objhandle = %p, maxcount = %lu, "
		     "count = %p", (int) session, object, maxcount, count);

	CHECKSESSION(session, se);

	if (! object || maxcount == 0)
		RET(C_FindObjects, CKR_ARGUMENTS_BAD);

	LOCK_MUTEX(se->mutex);

	for (; se->obj_search_index < se->obj_list_count;
						se->obj_search_index++) {
		if (search_object(&se->obj_list[se->obj_search_index],
				  se->search_attrs, se->search_attrs_count)) {
			object[rc++] = se->obj_search_index + 1;
			if (rc >= maxcount) {
				*count = rc;
				se->obj_search_index++;
				os_log_debug(logsys, "Found %u object%s",
					     rc, rc == 1 ? "" : "s");
				UNLOCK_MUTEX(se->mutex);
				RET(C_FindObjects, CKR_OK);
			}
		}
	}

	os_log_debug(logsys, "Found %u object%s", rc, rc == 1 ? "" : "s");
	*count = rc;

	UNLOCK_MUTEX(se->mutex);
	RET(C_FindObjects, CKR_OK);
}

CK_RV C_FindObjectsFinal(CK_SESSION_HANDLE session)
{
	struct session *se;
	int i;

	FUNCINITCHK(C_FindObjectsFinal);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	os_log_debug(logsys, "session = %d", (int) session);

	for (i = 0; i < se->search_attrs_count; i++)
		free(se->search_attrs[i].pValue);

	free(se->search_attrs);
	se->search_attrs = NULL;
	se->search_attrs_count = 0;

	UNLOCK_MUTEX(se->mutex);

	RET(C_FindObjectsFinal, CKR_OK);
}

/*
 * Routines to support encryption (we don't handle EncryptUpdate at this time)
 */

CK_RV C_EncryptInit(CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech,
		    CK_OBJECT_HANDLE object)
{
	struct session *se;
	const struct mechanism_map *mm;
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_EncryptInit);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	if (! mech) {
		os_log_debug(logsys, "mechanism pointer is NULL");
		RET(C_EncryptInit, CKR_MECHANISM_INVALID);
	}

	os_log_debug(logsys, "session = %d, mech = %d, key = %d",
		     (int) session, (int) mech->mechanism, (int) object);

	object--;

	if (object >= se->obj_list_count) {
		rv = CKR_KEY_HANDLE_INVALID;
		goto out;
	}

	if (se->state != NO_PENDING) {
		os_log_debug(logsys, "Crypto operation already pending");
		rv = CKR_OPERATION_ACTIVE;
		goto out;
	}

	/*
	 * Right now we assume only a public key can perform encryption
	 */

	if (se->obj_list[object].class != CKO_PUBLIC_KEY) {
		rv = CKR_KEY_TYPE_INCONSISTENT;
		goto out;
	}

	/*
	 * Make sure we actually have an identity backing this object
	 * (certificates from the Keychain certificate slot, for example,
	 * don't and can't actually do any crypto)
	 */

	if (! se->obj_list[object].id) {
		rv = CKR_ARGUMENTS_BAD;
		goto out;
	}

	/*
	 * Map our mechanism to the Apple Security framework information
	 * we need.
	 */

	mm = get_mechmap(mech->mechanism);

	/*
	 * Make sure we got a valid mechanism and that we can use it for
	 * encryption.
	 */

	if (! mm || (mm->usage_flags & CKF_ENCRYPT) == 0) {
		rv = CKR_MECHANISM_INVALID;
		goto out;
	}

	/*
	 * Validate the mechanism parameters.  The Apple Security framework
	 * doesn't really allow us to specify all crypto parameters for
	 * things like OAEP and PSS (specifically, the encoding parameter
	 * for OAEP salt lengths for PSS that do not match the hash size)
	 * So checking the mechanism parameters has two purposes.
	 * First, we want to map a particular mechanism/parameter
	 * combination to the algorithm specified in the Security framework.
	 * Secondly, if the parameters are ones that we don't support
	 * then we need to return an error.
	 */

	if (! mech_param_validate(mech, mm, &se->alg, NULL, NULL,
				  NULL)) {
		rv = CKR_MECHANISM_PARAM_INVALID;
		goto out;
	}

	if (se->key)
		CFRelease(se->key);

	LOCK_MUTEX(se->token->entry_mutex);
	se->key = se->obj_list[object].id->pubkey;
	CFRetain(se->key);
	UNLOCK_MUTEX(se->token->entry_mutex);

	if (mm->blocksize_out)
		se->outsize = SecKeyGetBlockSize(se->key);
	else
		se->outsize = 0;

	se->state = E_INIT;

out:
	UNLOCK_MUTEX(se->mutex);

	RET(C_EncryptInit, rv);
}

CK_RV C_Encrypt(CK_SESSION_HANDLE session, CK_BYTE_PTR indata,
		CK_ULONG indatalen, CK_BYTE_PTR outdata,
		CK_ULONG_PTR outdatalen)
{
	struct session *se;
	CFDataRef inref, outref;
	CFErrorRef err = NULL;
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_Encrypt);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	os_log_debug(logsys, "session = %d, indata = %p, inlen = %d, "
		     "outdata = %p, outlen = %d", (int) session, indata,
		     (int) indatalen, outdata, (int) *outdatalen);

	/*
	 * Make sure we have an in-progress operation
	 */

	if (se->state != E_INIT) {
		os_log_debug(logsys, "Encryption operation not initialized");
		UNLOCK_MUTEX(se->mutex);
		RET(C_Encrypt, CKR_OPERATION_NOT_INITIALIZED);
	}

	/*
	 * If we know our mechanism output size, check first to see if the
	 * output buffer is big enough.  Also, short-circuit this test if
	 * outdata is NULL.
	 */

	if (! outdata) {
		if (! se->outsize) {
			/* Hmm, what to do here?  No idea! */
			UNLOCK_MUTEX(se->mutex);
			RET(C_Encrypt, CKR_BUFFER_TOO_SMALL);
		}
		*outdatalen = se->outsize;
		os_log_debug(logsys, "outdata is NULL, returning an output "
			     "size of %d", (int) se->outsize);
		UNLOCK_MUTEX(se->mutex);
		RET(C_Encrypt, CKR_OK);
	}

	if (se->outsize && se->outsize > *outdatalen) {
		os_log_debug(logsys, "Output size is %d, but our output "
			     "buffer is %d", (int) se->outsize,
			     (int) *outdatalen);
		*outdatalen = se->outsize;
		UNLOCK_MUTEX(se->mutex);
		RET(C_Encrypt, CKR_BUFFER_TOO_SMALL);
	}

	inref = CFDataCreateWithBytesNoCopy(NULL, indata, indatalen,
					    kCFAllocatorNull);

	outref = SecKeyCreateEncryptedData(se->key, se->alg, inref,
					   &err);

	CFRelease(inref);

	if (! outref) {
		os_log_debug(logsys, "SecKeyCreateEncryptedData failed: "
			     "%{public}@ (%ld)", err,
			     (long) CFErrorGetCode(err));
		CFRelease(err);
		CFRelease(se->key);
		se->key = NULL;
		se->outsize = 0;
		se->state = NO_PENDING;
		UNLOCK_MUTEX(se->mutex);
		RET(C_Encrypt, CKR_GENERAL_ERROR);
	}

	if (*outdatalen < CFDataGetLength(outref)) {
		rv = CKR_BUFFER_TOO_SMALL;
	} else {
		memcpy(outdata, CFDataGetBytePtr(outref),
		       CFDataGetLength(outref));
		/*
		 * If the encryption was successful, release our key reference
		 */
		CFRelease(se->key);
		se->key = NULL;
		se->outsize = 0;
		se->state = NO_PENDING;
	}

	*outdatalen = CFDataGetLength(outref);

	CFRelease(outref);

	UNLOCK_MUTEX(se->mutex);

	RET(C_Encrypt, rv);
}

NOTSUPPORTED(C_EncryptUpdate, (CK_SESSION_HANDLE session, CK_BYTE_PTR inpart, CK_ULONG inpartlen, CK_BYTE_PTR outpart, CK_ULONG_PTR outpartlen))
NOTSUPPORTED(C_EncryptFinal, (CK_SESSION_HANDLE session, CK_BYTE_PTR lastpart, CK_ULONG_PTR lastpartlen))

/*
 * Routines to handle decryption.
 */

CK_RV C_DecryptInit(CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech,
		    CK_OBJECT_HANDLE key)
{
	struct session *se;
	const struct mechanism_map *mm;
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_DecryptInit);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	if (! mech) {
		os_log_debug(logsys, "mechanism pointer is NULL");
		RET(C_DecryptInit, CKR_MECHANISM_INVALID);
	}

	os_log_debug(logsys, "session = %d, mech = %d, key = %d",
		     (int) session, (int) mech->mechanism, (int) key);

	key--;

	if (key >= se->obj_list_count) {
		rv = CKR_KEY_HANDLE_INVALID;
		goto out;
	}

	/*
	 * Make sure no other operations are pending
	 */

	if (se->state != NO_PENDING) {
		os_log_debug(logsys, "Crypto operation already pending");
		rv = CKR_OPERATION_ACTIVE;
		goto out;
	}

	/*
	 * Right now we assume only a private key can perform decryption
	 */

	if (se->obj_list[key].class != CKO_PRIVATE_KEY) {
		rv = CKR_KEY_TYPE_INCONSISTENT;
		goto out;
	}

	if (! se->obj_list[key].id) {
		rv = CKR_ARGUMENTS_BAD;
		goto out;
	}

	if (! se->obj_list[key].id->privcandecrypt) {
		rv = CKR_KEY_FUNCTION_NOT_PERMITTED;
		goto out;
	}

	/*
	 * See the comments in C_EncryptInit() for what is going on here
	 */

	mm = get_mechmap(mech->mechanism);

	if (! mm || (mm->usage_flags & CKF_DECRYPT) == 0) {
		rv = CKR_MECHANISM_INVALID;
		goto out;
	}

	if (! mech_param_validate(mech, mm, &se->alg, NULL, NULL,
				  NULL)) {
		rv = CKR_MECHANISM_PARAM_INVALID;
		goto out;
	}

	if (se->key)
		CFRelease(se->key);

	LOCK_MUTEX(se->token->entry_mutex);
	se->key = se->obj_list[key].id->privkey;
	CFRetain(se->key);
	UNLOCK_MUTEX(se->token->entry_mutex);

	if (mm->blocksize_out)
		se->outsize = SecKeyGetBlockSize(se->key);
	else
		se->outsize = 0;

	se->state = D_INIT;

out:
	UNLOCK_MUTEX(se->mutex);

	RET(C_DecryptInit, rv);
}


CK_RV C_Decrypt(CK_SESSION_HANDLE session, CK_BYTE_PTR indata,
		CK_ULONG indatalen, CK_BYTE_PTR outdata,
		CK_ULONG_PTR outdatalen)
{
	struct session *se;
	CFDataRef inref, outref;
	CFErrorRef err = NULL;
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_Decrypt);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	os_log_debug(logsys, "session = %d, indata = %p, inlen = %d, "
		     "outdata = %p, outlen = %d", (int) session, indata,
		     (int) indatalen, outdata, (int) *outdatalen);

	/*
	 * Make sure we have an in-progress operation
	 */

	if (se->state != D_INIT) {
		os_log_debug(logsys, "Decrypt operation not initialized");
		UNLOCK_MUTEX(se->mutex);
		RET(C_Decrypt, CKR_OPERATION_NOT_INITIALIZED);
	}

	/*
	 * If we know our mechanism output size, check first to see if the
	 * output buffer is big enough.  Also, short-circuit this test if
	 * outdata is NULL.
	 *
	 * This is slightly more complicated when it comes to decryption,
	 * because the output length is variable.  But calling the decryption
	 * function multiple times can result in multiple pop-up dialog
	 * boxes for PIN requests.  So what I've come up with is if the
	 * outdata pointer is NULL (for a size probe) return the blocksize,
	 * which is the maximum output size for the decrypted data (given
	 * current algorithms we support).
	 */

	if (! outdata) {
		if (! se->outsize) {
			/* Hmm, what to do here?  No idea! */
			UNLOCK_MUTEX(se->mutex);
			RET(C_Decrypt, CKR_BUFFER_TOO_SMALL);
		}
		*outdatalen = se->outsize;
		os_log_debug(logsys, "outdata is NULL, returning an output "
			     "size of %d", (int) se->outsize);
		UNLOCK_MUTEX(se->mutex);
		RET(C_Decrypt, CKR_OK);
	}

	if (se->outsize && se->outsize > *outdatalen) {
		os_log_debug(logsys, "Output size is %d, but our output "
			     "buffer is %d", (int) se->outsize,
			     (int) *outdatalen);
		*outdatalen = se->outsize;
		UNLOCK_MUTEX(se->mutex);
		RET(C_Decrypt, CKR_BUFFER_TOO_SMALL);
	}

	inref = CFDataCreateWithBytesNoCopy(NULL, indata, indatalen,
					    kCFAllocatorNull);

	outref = SecKeyCreateDecryptedData(se->key, se->alg, inref,
					   &err);

	CFRelease(inref);

	if (! outref) {
		os_log_debug(logsys, "SecKeyCreateDecryptedData failed: "
			     "%{public}@ (%ld)", err,
			     (long) CFErrorGetCode(err));
		CFRelease(err);
		CFRelease(se->key);
		se->key = NULL;
		se->outsize = 0;
		UNLOCK_MUTEX(se->mutex);
		se->state = NO_PENDING;
		RET(C_Decrypt, CKR_GENERAL_ERROR);
	}

	if (*outdatalen < CFDataGetLength(outref)) {
		rv = CKR_BUFFER_TOO_SMALL;
	} else {
		memcpy(outdata, CFDataGetBytePtr(outref),
		       CFDataGetLength(outref));
		/*
		 * If the decryption was successful, release our key reference
		 */
		CFRelease(se->key);
		se->key = NULL;
		se->outsize = 0;
		se->state = NO_PENDING;
	}

	*outdatalen = CFDataGetLength(outref);

	CFRelease(outref);

	UNLOCK_MUTEX(se->mutex);

	RET(C_Decrypt, rv);
}

NOTSUPPORTED(C_DecryptUpdate, (CK_SESSION_HANDLE session, CK_BYTE_PTR inpart, CK_ULONG inpartlen, CK_BYTE_PTR outpart, CK_ULONG_PTR outpartlen))
NOTSUPPORTED(C_DecryptFinal, (CK_SESSION_HANDLE session, CK_BYTE_PTR lastpart, CK_ULONG_PTR lastpartlen))
NOTSUPPORTED(C_DigestInit, (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech))
NOTSUPPORTED(C_Digest, (CK_SESSION_HANDLE session, CK_BYTE_PTR indata, CK_ULONG indatalen, CK_BYTE_PTR digest, CK_ULONG_PTR digestlen))
NOTSUPPORTED(C_DigestUpdate, (CK_SESSION_HANDLE session, CK_BYTE_PTR indata, CK_ULONG indatalen))
NOTSUPPORTED(C_DigestKey, (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE key))
NOTSUPPORTED(C_DigestFinal, (CK_SESSION_HANDLE session, CK_BYTE_PTR digest, CK_ULONG_PTR digestlen))

/*
 * Start a signature operation.  Our global assumption is that the signature
 * is only done with a private key; if that changes then we need to change
 * this code.
 */

CK_RV C_SignInit(CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech,
		 CK_OBJECT_HANDLE object)
{
	struct session *se;
	const struct mechanism_map *mm;
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_SignInit);

	os_log_debug(logsys, "session = %d, mechanism = %s, object = %d",
		    (int) session, getCKMName(mech->mechanism), (int) object);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	/*
	 * Make sure no operations are in progress
	 */

	if (se->state != NO_PENDING) {
		os_log_debug(logsys, "Crypto operation already pending");
		rv = CKR_OPERATION_ACTIVE;
		goto out;
	}

	object--;

	if (object >= se->obj_list_count) {
		rv = CKR_KEY_HANDLE_INVALID;
		goto out;
	}

	if (! se->obj_list[object].id) {
		rv = CKR_ARGUMENTS_BAD;
		goto out;
	}

	if (! se->obj_list[object].id->privcansign) {
		rv = CKR_KEY_FUNCTION_NOT_PERMITTED;
		goto out;
	}

	/*
	 * Right now we are assuming only a private key can do signing.
	 * Change this assumption in the future if necessary
	 */

	if (se->obj_list[object].class != CKO_PRIVATE_KEY) {
		rv = CKR_KEY_TYPE_INCONSISTENT;
		goto out;
	}

	/*
	 * See the comments in C_EncryptInit() for what is going on here
	 */

	mm = get_mechmap(mech->mechanism);

	if (! mm || (mm->usage_flags & CKF_SIGN) == 0) {
		rv = CKR_MECHANISM_INVALID;
		goto out;
	}

	if (! mech_param_validate(mech, mm, NULL, &se->alg, &se->dalg,
				  &se->hash_alg)) {
		rv = CKR_MECHANISM_PARAM_INVALID;
		goto out;
	}

	/*
	 * Map our mechanism onto what we need for signing
	 */

	if (se->key)
		CFRelease(se->key);

	LOCK_MUTEX(se->token->entry_mutex);
	se->key = se->obj_list[object].id->privkey;
	CFRetain(se->key);
	UNLOCK_MUTEX(se->token->entry_mutex);

	if (mm->blocksize_out) {
		se->outsize = SecKeyGetBlockSize(se->key);
	} else {
		se->outsize = 0;
	}

	se->state = S_INIT;

out:
	UNLOCK_MUTEX(se->mutex);

	RET(C_SignInit, rv);
}

/*
 * Actually sign the data
 */
CK_RV C_Sign(CK_SESSION_HANDLE session, CK_BYTE_PTR indata, CK_ULONG indatalen,
	     CK_BYTE_PTR sig, CK_ULONG_PTR siglen)
{
	struct session *se;
	CFDataRef inref, outref;
	CFErrorRef err = NULL;
	CK_RV rv = CKR_OK;
#ifdef KEYCHAIN_DEBUG
	char *file;
#endif /* KEYCHAIN_DEBUG */

	FUNCINITCHK(C_Sign);

	os_log_debug(logsys, "session = %d, indata = %p, inlen = %d, "
		     "outdata = %p, outlen = %d", (int) session, indata,
		     (int) indatalen, sig, (int) *siglen);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

#ifdef KEYCHAIN_DEBUG
	if ((file = getenv("KEYCHAIN_PKCS11_SIGN_DATAFILE"))) {
		FILE *f = fopen(file, "w");

		if (! f) {
			os_log_debug(logsys, "Failed to open \"%s\": "
				     "%{darwin.errno}d", file, errno);
		} else {
			fwrite(indata, indatalen, 1, f);
			fclose(f);
		}
	}
#endif /* KEYCHAIN_DEBUG */

	/*
	 * Make sure the signing operation is initialized
	 */

	if (se->state != S_INIT) {
		os_log_debug(logsys, "Sign operation not initialized");
		rv = CKR_OPERATION_NOT_INITIALIZED;
		goto out;
	}

	/*
	 * If we know our mechanism output size, check first to see if the
	 * output buffer is big enough.  Also, short-circuit this test if
	 * sig is NULL.
	 */

	if (! sig) {
		if (! se->outsize) {
			/* Hmm, what to do here?  No idea! */
			rv = CKR_BUFFER_TOO_SMALL;
			goto out;
		}
		*siglen = se->outsize;
		os_log_debug(logsys, "sig is NULL, returning an output "
			     "size of %d", (int) se->outsize);
		goto out;
	}

	if (se->outsize && se->outsize > *siglen) {
		os_log_debug(logsys, "Output size is %d, but our output "
			     "buffer is %d", (int) se->outsize, (int) *siglen);
		*siglen = se->outsize;
		rv = CKR_BUFFER_TOO_SMALL;
		goto out;
	}

	inref = CFDataCreateWithBytesNoCopy(NULL, indata, indatalen,
					    kCFAllocatorNull);

	outref = SecKeyCreateSignature(se->key, se->alg, inref, &err);

	CFRelease(inref);

	if (! outref) {
		os_log_debug(logsys, "SecKeyCreateSignature failed: "
			     "%{public}@", err);
		CFRelease(err);
		CFRelease(se->key);
		se->key = NULL;
		se->outsize = 0;
		se->state = NO_PENDING;
		UNLOCK_MUTEX(se->mutex);
		RET(C_Sign, CKR_GENERAL_ERROR);
	}

	if (*siglen < CFDataGetLength(outref)) {
		rv = CKR_BUFFER_TOO_SMALL;
	} else {
		memcpy(sig, CFDataGetBytePtr(outref), CFDataGetLength(outref));
		/*
		 * If the signature was successful, release our key reference
		 */
		CFRelease(se->key);
		se->key = NULL;
		se->outsize = 0;
		se->state = NO_PENDING;
	}

	*siglen = CFDataGetLength(outref);

	CFRelease(outref);

#if KEYCHAIN_DEBUG
	if ((file = getenv("KEYCHAIN_PKCS11_SIGN_SIGFILE"))) {
		FILE *f = fopen(file, "w");

		if (! f) {
			os_log_debug(logsys, "Failed to open \"%s\": "
				     "%{darwin.errno}d", file, errno);
		} else {
			fwrite(sig, *siglen, 1, f);
			fclose(f);
		}
	}
#endif /* KEYCHAIN_DEBUG */

out:
	UNLOCK_MUTEX(se->mutex);

	RET(C_Sign, rv);
}

/*
 * Support a multi-part signature operation
 */

CK_RV C_SignUpdate(CK_SESSION_HANDLE session, CK_BYTE_PTR indata,
		   CK_ULONG indatalen)
{
	struct session *se;
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_SignUpdate);

	os_log_debug(logsys, "session = %d, indata = %p, inlen = %d",
		     (int) session, indata, (int) indatalen);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	/*
	 * Make sure we are either in S_INIT or S_UPDATE
	 */

	if (se->state != S_INIT && se->state != S_UPDATE) {
		os_log_debug(logsys, "Not in S_INIT or S_UPDATE state");
		rv = CKR_OPERATION_NOT_INITIALIZED;
		goto out;
	}

	/*
	 * There are some signature operations that cannot take a
	 * multi-part operation.  The big one is CKM_RSA_PKCS, but
	 * there are others.
	 *
	 * For these, there is no "digest" algorithm available.  So if
	 * the dalg is set to NULL, return an error.  CKR_DATA_LEN_RANGE
	 * is the best one I can think of right now.
	 */

	if (!se->dalg) {
		rv = CKR_DATA_LEN_RANGE;
		goto out;
	}

	/*
	 * If we don't have a pending signature operation, then
	 * start that now.
	 *
	 * We USED to use the SecTransform API, but it turns out that
	 * a relatively normal digest algorithm API is available via
	 * CommonCrypto.  We use our wrapper on top of CommonCrypto
	 * so we can be algorithm-agnostic.
	 */

	if (se->state == S_INIT) {
		/*
		 * Because there's not really an API that will let us
		 * do a multi-part signature operation, we have to calculate
		 * the digest ourselves then call the appropriate "digest"
		 * signing function.  SecSignTransformCreate() doesn't work
		 * when the key is on a smartcard.
		 */

		if (! cc_md_init(se->hash_alg, &se->mdc)) {
			os_log_debug(logsys, "Unable to initialize digest "
				     "function for %s",
				     getCKMName(se->hash_alg));
			rv = CKR_GENERAL_ERROR;		
			se->state = NO_PENDING;
			goto out;
		}
		se->state = S_UPDATE;
	}

	cc_md_update(se->mdc, indata, indatalen);

out:
	UNLOCK_MUTEX(se->mutex);

	RET(C_SignUpdate, rv);
}

/*
 * Finalize the signature
 */

CK_RV C_SignFinal(CK_SESSION_HANDLE session, CK_BYTE_PTR sig,
		  CK_ULONG_PTR siglen)
{
	struct session *se;
	CFErrorRef err = NULL;
	unsigned char *digest = NULL;
	unsigned int digest_len;
	CFDataRef sigout = NULL, datain = NULL;
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_SignFinal);

	os_log_debug(logsys, "session = %d, sig = %p, siglen = %d",
		     (int) session, sig, (int) *siglen);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	/*
	 * Make sure we are in S_UPDATE (C_SignUpdate() has been called
	 * at least once
	 */

	if (se->state != S_UPDATE) {
		os_log_debug(logsys, "Not in S_UPDATE state");
		rv = CKR_OPERATION_NOT_INITIALIZED;
		goto out;
	}

	/*
	 * We have to deal with the case where the caller calls us to probe
	 * the appropriate output buffer size, so we might get called multiple
	 * times.
	 *
	 * As of this writing, all crypto mechanisms we support have
	 * an output size based on the key block size.  So if we get called
	 * to probe the output buffer size, then we return the appropriate
	 * block size.
	 */

	if (! sig) {
		if (! se->outsize) {
			/* Hmm, what to do here?  No idea! */
			rv = CKR_BUFFER_TOO_SMALL;
			goto out;
		}
		*siglen = se->outsize;
		os_log_debug(logsys, "sig is NULL, returning an output "
			     "size of %d", (int) se->outsize);
		goto out;
	}

	if (se->outsize && se->outsize > *siglen) {
		os_log_debug(logsys, "Output size is %d, but our output "
			     "buffer is %d", (int) se->outsize, (int) *siglen);
		*siglen = se->outsize;
		rv = CKR_BUFFER_TOO_SMALL;
		goto out;
	}

	/*
	 * Finalize the digest operation.
	 */

	cc_md_final(se->mdc, &digest, &digest_len);
	se->mdc = NULL;

	/*
	 * Pass the digested data into the SecKeyCreateSignature
	 * function.  Note that we use the "digest" algorithm
	 */

	datain = CFDataCreateWithBytesNoCopy(NULL, digest, digest_len,
					     kCFAllocatorNull);

	if (! datain) {
		os_log_debug(logsys, "Unable to create digest CFData");
		goto outfinish;
	}

	/*
	 * Create the actual signature based on the digested data
	 */

	sigout = SecKeyCreateSignature(se->key, se->dalg, datain, &err);

	if (! sigout) {
		os_log_debug(logsys, "SecKeyCreateSignature failed: "
				     "%{public}@", err);
		CFRelease(err);
		rv = CKR_FUNCTION_FAILED;
		goto outfinish;
	}

	if (*siglen < CFDataGetLength(sigout)) {
		/*
		 * This shouldn't happen, but if it does treat it
		 * as an internal error for now.
		 */

		os_log_debug(logsys, "Our output buffer size is %d, "
			     "but the output data is %d bytes!",
			     (int) *siglen, (int) CFDataGetLength(sigout));
		rv = CKR_FUNCTION_FAILED;
		goto out;
	}

	*siglen = CFDataGetLength(sigout);
	CFDataGetBytes(sigout, CFRangeMake(0, *siglen), sig);

outfinish:
	if (digest)
		free(digest);
	if (datain)
		CFRelease(datain);
	if (sigout)
		CFRelease(sigout);
	CFRelease(se->key);
	se->key = NULL;
	se->dalg = NULL;
	se->alg = NULL;
	se->outsize = 0;
	se->state = NO_PENDING;

out:
	UNLOCK_MUTEX(se->mutex);

	RET(C_SignFinal, rv);
}

NOTSUPPORTED(C_SignRecoverInit, (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech, CK_OBJECT_HANDLE key))
NOTSUPPORTED(C_SignRecover, (CK_SESSION_HANDLE session, CK_BYTE_PTR indata, CK_ULONG indatalen, CK_BYTE_PTR sig, CK_ULONG_PTR siglen))

CK_RV C_VerifyInit(CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech,
		   CK_OBJECT_HANDLE key)
{
	struct session *se;
	const struct mechanism_map *mm;
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_VerifyInit);

	os_log_debug(logsys, "session = %d, mechanism = %s, object = %d",
		    (int) session, getCKMName(mech->mechanism), (int) key);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	/*
	 * Make sure no operations are in progress
	 */

	if (se->state != NO_PENDING) {
		os_log_debug(logsys, "Crypto operation already pending");
		rv = CKR_OPERATION_ACTIVE;
		goto out;
	}

	key--;

	if (key >= se->obj_list_count) {
		rv = CKR_KEY_HANDLE_INVALID;
		goto out;
	}

	if (! se->obj_list[key].id) {
		rv = CKR_ARGUMENTS_BAD;
		goto out;
	}
		
	if (! se->obj_list[key].id->pubcanverify) {
		rv = CKR_KEY_FUNCTION_NOT_PERMITTED;
		goto out;
	}

	if (se->obj_list[key].class != CKO_PUBLIC_KEY) {
		rv = CKR_KEY_TYPE_INCONSISTENT;
		goto out;
	}

	/*
	 * See the comments in C_EncryptInit() for what is going on here
	 */

	mm = get_mechmap(mech->mechanism);

	if (! mm || (mm->usage_flags & CKF_VERIFY) == 0) {
		rv = CKR_MECHANISM_INVALID;
		goto out;
	}

	if (! mech_param_validate(mech, mm, NULL, &se->alg, &se->dalg,
				  &se->hash_alg)) {
		rv = CKR_MECHANISM_PARAM_INVALID;
		goto out;
	}

	/*
	 * Map our mechanism onto what we need for signing
	 */

	if (se->key)
		CFRelease(se->key);

	se->key = se->obj_list[key].id->pubkey;
	CFRetain(se->key);

	if (mm->blocksize_out) {
		se->outsize = SecKeyGetBlockSize(se->key);
	} else {
		se->outsize = 0;
	}

	se->state = V_INIT;

out:
	UNLOCK_MUTEX(se->mutex);

	RET(C_VerifyInit, rv);
}

CK_RV C_Verify(CK_SESSION_HANDLE session, CK_BYTE_PTR indata,
	       CK_ULONG indatalen, CK_BYTE_PTR sig, CK_ULONG siglen)
{
	struct session *se;
	CFDataRef inref, sigref;
	CFErrorRef err = NULL;
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_Verify);

	os_log_debug(logsys, "session = %d, indata = %p, inlen = %d, "
		     "outdata = %p, outlen = %d", (int) session, indata,
		     (int) indatalen, sig, (int) siglen);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	if (se->state != V_INIT) {
		os_log_debug(logsys, "No Verify operation initialized");
		rv = CKR_OPERATION_NOT_INITIALIZED;
		goto out;
	}

	inref = CFDataCreateWithBytesNoCopy(NULL, indata, indatalen,
					    kCFAllocatorNull);
	sigref = CFDataCreateWithBytesNoCopy(NULL, sig, siglen,
					     kCFAllocatorNull);

	if (!SecKeyVerifySignature(se->key, se->alg, inref, sigref,
				   &err)) {
		os_log_debug(logsys, "VerifySignature failed: %{public}@", err);
		CFRelease(err);
		rv = CKR_SIGNATURE_INVALID;
	}

	/*
	 * Always release the key reference at this point
	 */

	CFRelease(se->key);
	se->key = NULL;
	CFRelease(inref);
	CFRelease(sigref);
	se->state = NO_PENDING;

out:
	UNLOCK_MUTEX(se->mutex);

	RET(C_Verify, rv);
}

CK_RV C_VerifyUpdate(CK_SESSION_HANDLE session, CK_BYTE_PTR indata,
		     CK_ULONG indatalen)
{
	struct session *se;
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_VerifyUpdate);

	os_log_debug(logsys, "session = %d, indata = %p, indatalen = %d",
		     (int) session, indata, (int) indatalen);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	/*
	 * Make sure we are in V_INIT or V_UPDATE
	 */

	if (se->state != V_INIT && se->state != V_UPDATE) {
		os_log_debug(logsys, "Not in V_INIT or V_UPDATE state");
		rv = CKR_OPERATION_NOT_INITIALIZED;
		goto out;
	}

	/*
	 * Make sure we can actually use the Update function; see
	 * C_SignUpdate for an explanation.
	 */

	if (!se->dalg) {
		rv = CKR_DATA_LEN_RANGE;
		goto out;
	}

	if (se->state == V_INIT) {
		/*
		 * See the comments in C_SignUpdate for what is going on
		 * in this section.
		 */

		if (! cc_md_init(se->hash_alg, &se->mdc)) {
			os_log_debug(logsys, "Unable to initialize digest "
				     "function for %s",
				     getCKMName(se->hash_alg));
			rv = CKR_GENERAL_ERROR;		
			se->state = NO_PENDING;
			goto out;
		}
		se->state = V_UPDATE;
	}

	cc_md_update(se->mdc, indata, indatalen);

out:
	UNLOCK_MUTEX(se->mutex);

	RET(C_VerifyUpdate, rv);
}


CK_RV C_VerifyFinal(CK_SESSION_HANDLE session, CK_BYTE_PTR sig,
		    CK_ULONG siglen)
{
	struct session *se;
	CFDataRef sigdata = NULL, digest_data = NULL;
	CFErrorRef err = NULL;
	unsigned char *digest = NULL;
	unsigned int digest_len;
	CK_RV rv = CKR_OK;

	FUNCINITCHK(C_VerifyFinal);

	os_log_debug(logsys, "session = %d, sig = %p, siglen = %d",
		     (int) session, sig, (int) siglen);

	CHECKSESSION(session, se);

	LOCK_MUTEX(se->mutex);

	if (se->state != V_UPDATE) {
		os_log_debug(logsys, "Not in V_UPDATE state");
		rv = CKR_OPERATION_NOT_INITIALIZED;
		goto out;
	}

	sigdata = CFDataCreateWithBytesNoCopy(NULL, sig, siglen,
					      kCFAllocatorNull);

	/*
	 * At least this is simpler than C_SignFinal.  Finalize the
	 * hash function and verify the signature (using the digest
	 * algorithm).
	 */

	cc_md_final(se->mdc, &digest, &digest_len);
	se->mdc = NULL;

	digest_data = CFDataCreateWithBytesNoCopy(NULL, digest, digest_len,
						  kCFAllocatorNull);

	if (!SecKeyVerifySignature(se->key, se->dalg, digest_data, sigdata,
				   &err)) {
		os_log_debug(logsys, "VerifySignature failed: %{public}@", err);
		CFRelease(err);
		rv = CKR_SIGNATURE_INVALID;
	}

out:
	if (sigdata)
		CFRelease(sigdata);
	if (digest_data)
		CFRelease(digest_data);
	if (digest)
		free(digest);

	UNLOCK_MUTEX(se->mutex);

	RET(C_VerifyFinal, rv);
}
NOTSUPPORTED(C_VerifyRecoverInit, (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech, CK_OBJECT_HANDLE key))
NOTSUPPORTED(C_VerifyRecover, (CK_SESSION_HANDLE session, CK_BYTE_PTR sig, CK_ULONG siglen, CK_BYTE_PTR outdata, CK_ULONG_PTR outdatalen))
NOTSUPPORTED(C_DigestEncryptUpdate, (CK_SESSION_HANDLE session, CK_BYTE_PTR inpart, CK_ULONG inpartlen, CK_BYTE_PTR outdata, CK_ULONG_PTR outdatalen))
NOTSUPPORTED(C_DecryptDigestUpdate, (CK_SESSION_HANDLE session, CK_BYTE_PTR indata, CK_ULONG indatalen, CK_BYTE_PTR outdata, CK_ULONG_PTR outdatalen))
NOTSUPPORTED(C_SignEncryptUpdate, (CK_SESSION_HANDLE session, CK_BYTE_PTR inpart, CK_ULONG inpartlen, CK_BYTE_PTR outdata, CK_ULONG_PTR outdatalen))
NOTSUPPORTED(C_DecryptVerifyUpdate, (CK_SESSION_HANDLE session, CK_BYTE_PTR indata, CK_ULONG indatalen, CK_BYTE_PTR outdata, CK_ULONG_PTR outdatalen))
NOTSUPPORTED(C_GenerateKey, (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech, CK_ATTRIBUTE_PTR template, CK_ULONG count, CK_OBJECT_HANDLE_PTR key))
NOTSUPPORTED(C_GenerateKeyPair, (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech, CK_ATTRIBUTE_PTR pub_template, CK_ULONG pub_count, CK_ATTRIBUTE_PTR priv_template, CK_ULONG priv_count, CK_OBJECT_HANDLE_PTR pubkey, CK_OBJECT_HANDLE_PTR privkey))
NOTSUPPORTED(C_WrapKey, (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech, CK_OBJECT_HANDLE wrapkey, CK_OBJECT_HANDLE key, CK_BYTE_PTR outkey, CK_ULONG_PTR outkeylen));
NOTSUPPORTED(C_UnwrapKey, (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech, CK_OBJECT_HANDLE unwrapkey, CK_BYTE_PTR wrappedkey, CK_ULONG wrappedkeylen, CK_ATTRIBUTE_PTR template, CK_ULONG count, CK_OBJECT_HANDLE_PTR outkey))
NOTSUPPORTED(C_DeriveKey, (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mech, CK_OBJECT_HANDLE basekey, CK_ATTRIBUTE_PTR template, CK_ULONG count, CK_OBJECT_HANDLE_PTR outkey))
NOTSUPPORTED(C_SeedRandom, (CK_SESSION_HANDLE session, CK_BYTE_PTR seed, CK_ULONG seedlen))
NOTSUPPORTED(C_GenerateRandom, (CK_SESSION_HANDLE session, CK_BYTE_PTR randomdata, CK_ULONG randomlen))
NOTSUPPORTED(C_GetFunctionStatus, (CK_SESSION_HANDLE session))
NOTSUPPORTED(C_CancelFunction, (CK_SESSION_HANDLE session))
NOTSUPPORTED(C_WaitForSlotEvent, (CK_SESSION_HANDLE session, CK_SLOT_ID_PTR slot_id, CK_VOID_PTR reserved))

/*
 * Use the Security framework to scan for any identities that are provided
 * by a smartcard, and copy out useful information from them.
 *
 * So, how does this work?
 *
 * What we USED to do was call SecItemCopyMatching() to find all identities
 * that matched a hardware token and glom them into one "slot".  But that
 * had some limitations.
 *
 * The new way is we get called at token insertion time as part of the
 * TKTokenWatcher callback with a specific token identifier.  We try to
 * add all of the token identities at that time.
 */ 

void
add_token_id(CFStringRef tokenid)
{
	CFMutableDictionaryRef query = NULL;
	CFTypeRef result = NULL;
	unsigned int i, count;
	OSStatus ret;
	struct slot_entry *token;

	/*
	 * Our keys to create our query dictionary.
	 *
	 * Here's what's the query dictionary means:
	 *
	 * kSecClass = kSecClassIdentity
	 *	This means we're searching for "identities" (certificates
	 *	with corresponding private key objects).
	 * kSecMatchLimit = kSecMatchLimitAll
	 *	Without this, we only get one identity.  Setting this means
	 *	our return value could be a list of identities.  If we get
	 *	more than one identity returned then the result will be
	 *	a CFArrayRef, otherwise it will be a CFDictionaryRef
	 *	(see below).
	 * kSecAttrAccessGroup = kSecAttrAccessGroupToken
	 *	This will limit the search to identities which are in the
	 *	"Token" Access group; this means smartcards.  This isn't
	 *	documented very well, but I see that the security tool
	 *	"list-smartcards" command uses this so I feel it's pretty
	 *	safe to rely on this search key for now.
	 * kSecAttrTokenID = token identifier
	 *	This will limit the search to hardware tokens which match
	 *	this specific token identifier.  The token identifier
	 *	we use comes from the TKTokenWatcher insertion handler.
	 * kSecReturnPersistentRef = kCFBooleanTrue
	 *	This means return a "persisistent" reference to the identity
	 *      (in a CFDataRef).  In earlier versions we would use
	 *      kSecReturnRef to get the SecIdentityRef, but we need to
	 *      bind a LAContext to the identity, and we do that by
	 *      using the persistent ref to retrieve the ACTUAL SecIdentityRef
	 *      and feeding in the LAContext into that query by using
	 *	kSecUseAUthenticationContext.  That actually happens later
	 *	in add_identity().  See comments in localauth.m for more
	 *	information.  The persistent ref ends up in the dictionary
	 *	under the kSecValuePersistentRef.
	 * kSecReturnAttributes = kCFBooleanTrue
	 *	This means we return all of the attributes for each identity.
	 *	We can use this to get access to things like the label
	 *	for the identity.
	 *
	 * Because we ask for all of the Attributes using kSecReturnAttributes
	 * the return value is a CFDictionaryRef containing all of the
	 * various attributes.  If we get more than one identity back, then
	 * we will get a CFArrayRef, with each entry in the array containing
	 * the CFDictionaryRef for that attribute.
	 *
	 * Whew.
	 */

	os_log_debug(logsys, "Looking for identities for token "
		     "%{public}@", tokenid);

	/*
	 * Create the query dictionary for SecCopyItemMatching(); see above.
	 * If the first call to add_dict succeeds then all of the rest will.
	 */

	if (! add_dict(&query, kSecClass, kSecClassIdentity))
		return;
	add_dict(&query, kSecMatchLimit, kSecMatchLimitAll);
	add_dict(&query, kSecAttrAccessGroup, kSecAttrAccessGroupToken);
	add_dict(&query, kSecAttrTokenID, tokenid);
	add_dict(&query, kSecReturnPersistentRef, kCFBooleanTrue);
	add_dict(&query, kSecReturnAttributes, kCFBooleanTrue);

	/*
	 * This is where the actual query happens
	 */

	ret = SecItemCopyMatching(query, &result);

	CFRelease(query);

	/*
	 * If we get an error, then just return and don't create
	 * this token.
	 */

	if (ret) {
		/*
		 * Handle the case where we just don't see any matching
		 * results.  Which is really identical to the "error"
		 * case as well.
		 */

		if (ret == errSecItemNotFound) {
			os_log_debug(logsys, "No identities found");
		} else {
			LOG_SEC_ERR("SecItemCopyMatching failed: "
				    "%{public}@", ret);
		}
		goto out;
	}

	count = cflistcount(result);

	/*
	 * Allocate our slot entry now and allocate a new
	 * LocalAuthentication context for it.
	 */

	token = malloc(sizeof(*token));

	memset(token, 0, sizeof(*token));

	token->tokenid = tokenid;
	CFRetain(token->tokenid);
	token->id_list = NULL;
	token->id_count = 0;
	token->id_size = 0;
	token->logged_in = false;
	token->label = NULL;
	token->lacontext = lacontext_new();
	CREATE_MUTEX(token->entry_mutex);
	token->refcount = 1;

	os_log_debug(logsys, "%u identities found", count);

	for (i = 0; i < count; i++)  {
		os_log_debug(logsys, "Copying identity %u", i + 1);

		if (add_identity(token, cfgetindex(result, i)) == -1)
			os_log_debug(logsys, "Adding identity %u "
				     "failed", i + 1);
	}

	/*
	 * If we didn't have any identities added, then this token
	 * isn't valid.  Just free it.
	 */

	if (token->id_count == 0) {
		os_log_debug(logsys, "No identities added, not creating token");
		slot_entry_free(token, false);
		goto out;
	}

	/*
	 * Build the objects for these identities
	 */

	build_id_objects(token);

	/*
	 * Now that we have a valid entry, time to add it to our slot list.
	 * See if we have an open slot list entry.  If not, then make our
	 * slot list a little bigger and add one
	 */

	LOCK_MUTEX(slot_mutex);
	for (i = 0; i < slot_count; i++) {
		if (slot_list[i] == NULL) {
			goto got_slot;
		}
	}

	slot_count++;

	slot_list = realloc(slot_list, sizeof(*slot_list) * slot_count);

got_slot:
	/*
	 * Bring over the token label, which is just going to be the
	 * first identity label.
	 */

	token->label = strdup(token->id_list[0]->label);

	os_log_debug(logsys, "Adding new token at slot %u", i);
	slot_list[i] = token;

	UNLOCK_MUTEX(slot_mutex);

out:
	if (result)
		CFRelease(result);
	return;
}

/*
 * Add an identity to our identity list.  Takes a CFDictionaryRef with
 * all of the identity attributes (and persistent reference) in it.
 */

static int
add_identity(struct slot_entry *entry, CFDictionaryRef dict)
{
	CFStringRef label;
	CFNumberRef keytype;
	CFTypeRef refresult;
	CFMutableDictionaryRef refquery = NULL;
	CFDictionaryRef keydict;
	CFDataRef p_ref;
	OSStatus ret;
	struct id_info *id;

	/*
	 * Just in case ...
	 */

	if (dict == NULL) {
		os_log_debug(logsys, "Identity dictionary is NULL, returning!");
		return -1;
	}

	if (! CFDictionaryGetValueIfPresent(dict, kSecValuePersistentRef,
					    (const void **)&p_ref)) {
		os_log_debug(logsys, "Persistent id reference not found");
		return -1;
	}

	/*
	 * Our query dictionary for SecItemCopyMatching.  Here are the
	 * components of our query dictionary:
	 *
	 * kSecClass = kSecClassIdentity
	 *	See scan_identities() for more details, but this limits us to
	 *	only retrieving identities.
	 * kSecMatchLimit = kSecMatchLimitOne
	 *	Because we're using the persistent reference (see below)
	 *	we really only want one response (and we should only get one)
	 *	so set the match limit to one so we only get back a single
	 *	response.
	 * kSecReturnRef = kCFBooleanTrue
	 *	We set this to indicate that we want an identity reference
	 *	back (SecIdentityRef); this should be the only thing returned
	 *	because we set no other return keys and we are requesting
	 *	only one.
	 * kSecUseAuthenticationContext = LAContext
	 *	This is covered in more detail in localauth.m, but the idea
	 *	here is this is a Local Authentication context created by
	 *	the LAContext class.  When converting from a persistent ref
	 *	to a SecIdentityRef and an auth context is passed in via
	 *	kSecUseAuthenticationContext, it is converted by the Security
	 *	framework internally to an ACM context and we can then later
	 *	use the LAContext methods to authentication to the token.
	 * kSecValuePersistentRef = persistent reference
	 *	This is the persistent reference generated in the attribute
	 *	dictionary when we did the search for all identities in
	 *	scan_identities().  The reason we get a persistent reference
	 *	in scan_identities() and then convert it to a "real" reference
	 *	(e.g., SecIdentityRef) is so we can bind the LAContext
	 *	to the identity so we have the ability to input the PIN
	 *	via the PKCS#11 API (rather than let the Security framework
	 *	ask for it).  It is worth noting that there is ALSO an
	 *	attribute key called "kSecAttrPersistentReference"; as far
	 *	as I can tell, that key is not used for anything.  The
	 *	Apple documentation says to convert a persistent reference
	 *	to a normal reference you should pass in the persistent
	 *	reference in a CFArray using the kSecMatchItemList, but
	 *	I can definitely say that at least for me, this did not work.
	 */

	/*
	 * Build up our query dictionary (see above comments).  Feed in
	 * the persistent reference to extract the REAL identity reference
	 * (SecIdentityRef).  If we also pass in the LAContext this will
	 * attach the LAContext to the identity.
	 */

	if (! add_dict(&refquery, kSecClass, kSecClassIdentity))
		return -1;
	add_dict(&refquery, kSecMatchLimit, kSecMatchLimitOne);
	add_dict(&refquery, kSecReturnRef, kCFBooleanTrue);
	add_dict(&refquery, kSecValuePersistentRef, p_ref);

	/*
	 * It turns out that in some cases (if you are running under
	 * 32 bit mode, for example), you won't be able to allocate a
	 * lacontext.  So if that happens we need to be sure not to
	 * put the lacontext into our dictionary.  We can't use kCFNull
	 * because that will cause the SecItemCopyMatching call later
	 * to fail, so if we don't have a lacontext then don't add it to
	 * the query dictionary.
	 */

	if (entry->lacontext)
		add_dict(&refquery, kSecUseAuthenticationContext,
			 entry->lacontext);

	ret = SecItemCopyMatching(refquery, &refresult);

	CFRelease(refquery);

	if (ret) {
		LOG_SEC_ERR("Persistent ref SecItemCopyMatching "
			    "failed: %{public}@", ret);
		return -1;
	}

	if (CFGetTypeID(refresult) != SecIdentityGetTypeID()) {
		logtype("Was expecting a SecIdentityRef, but got", refresult);
		CFRelease(refresult);
		return -1;
	}

	id = malloc(sizeof(*id));
	memset(id, 0, sizeof(*id));

	id->ident = NULL;
	id->cert = NULL;
	id->privkey = NULL;
	id->pubkey = NULL;
	id->label = NULL;
	id->secaccess = NULL;
	id->pkeyhash = NULL;

	/*
	 * No need to retain; we own this as a result of it coming out of
	 * SecItemCopyMatching
	 */

	id->ident = (SecIdentityRef) refresult;

	/*
	 * Extract out of the dictionary all of the things we need.
	 * Note that since we are following the "Get Rule" and this
	 * dictionary should be de-allocated soon, we need to CFRetain()
	 * everything we want for later.
	 *
	 * Key items:
	 *
	 * Attribute label (display string for the identity)
	 * SecIdentityRef (used by Security Framework)
	 * Various attribute flags (we use those for returning
	 * object information)
	 *
	 * To make things easier, we extract the private key object
	 * and the certificate from the identity.  Those are copies and
	 * we don't need to retain those objects.
	 */

	if (CFDictionaryGetValueIfPresent(dict, kSecAttrLabel,
					  (const void **) &label)) {
		id->label = getstrcopy(label);
		os_log_debug(logsys, "Identity label: %{public}@", label);
	} else {
		id->label = strdup("Hardware token");
		os_log_debug(logsys, "No label, using default");
	}
	
	if (! CFDictionaryGetValueIfPresent(dict, kSecAttrKeyType,
					    (const void **) &keytype)) {
		os_log_debug(logsys, "Key type not found");
		ret = -1;
		goto out;
	}

	id->keytype = convert_keytype(keytype);

	if (! CFDictionaryGetValueIfPresent(dict, kSecAttrPublicKeyHash,
					    (const void **) &id->pkeyhash)) {
		os_log_debug(logsys, "Public key hash not found");
		ret = -1;
		goto out;
	}

	CFRetain(id->pkeyhash);

	id->privcansign = boolfromdict("Can-Sign", dict, kSecAttrCanSign);
	id->privcandecrypt = boolfromdict("Can-Decrypt", dict,
					  kSecAttrCanDecrypt);

	ret = SecIdentityCopyCertificate(id->ident, &id->cert);

	if (ret)
		LOG_SEC_ERR("CopyCertificate failed: %{public}@", ret);

	if (! ret) {
		ret = SecIdentityCopyPrivateKey(id->ident, &id->privkey);
		if (ret)
			LOG_SEC_ERR("CopyPrivateKey failed: %{public}@", ret);
		else {
			if (! (id->secaccess = getaccesscontrol(dict))) {
				ret = -1;
				goto out;
			}
		}
	}

	if ( !ret) {
		ret = SecCertificateCopyPublicKey(id->cert, &id->pubkey);
		if (ret)
			LOG_SEC_ERR("CopyPublicKey failed: %{public}@", ret);
	}

	/*
	 * Get our public key attributes
	 */

	if (! ret) {
		keydict = SecKeyCopyAttributes(id->pubkey);

		id->pubcanverify = boolfromdict("Can-Verify", keydict,
						kSecAttrCanVerify);
		id->pubcanencrypt = boolfromdict("Can-Encrypt", keydict,
						 kSecAttrCanEncrypt);
		id->pubcanwrap = boolfromdict("Can-Wrap", keydict,
					      kSecAttrCanWrap);
		/*
		 * We're going to cheat here JUST a bit.  It turns out
		 * if a public key is set to allow wrapping, it can also
		 * do generic encryption.  So if we have wrapping set, also
		 * set encryption.
		 */

		if (id->pubcanwrap)
			id->pubcanencrypt = true;

		CFRelease(keydict);
	}

out:
	if (ret) {
		id_info_free(id);
		return -1;
	}

	/*
	 * If we don't have enough id entries, allocate some more.
	 */

	if (++entry->id_count > entry->id_size) {
		entry->id_size += 5;
		entry->id_list = realloc(entry->id_list,
					 sizeof(*(entry->id_list)) *
					 		entry->id_size);
	}

	entry->id_list[entry->id_count - 1] = id;

	return 0;
}

/*
 * Remove a token from a slot
 */

void
remove_token_id(CFStringRef tokenid)
{
	int i;

	os_log_debug(logsys, "Received removal event for token %{public}@",
		     tokenid);

	LOCK_MUTEX(slot_mutex);

	/*
	 * Go through the list and remove whatever token we match on.
	 * Because we're doing refcounting we shouldn't free any memory
	 * that is being used by a session.
	 */

	for (i = 0; i < slot_count; i++) {
		if (slot_list[i] && CFEqual(tokenid, slot_list[i]->tokenid)) {
			os_log_debug(logsys, "Removing token from slot %d", i);
			slot_entry_free(slot_list[i], false);
			slot_list[i] = NULL;
			break;
		}
	}

	if (i == slot_count)
		os_log_debug(logsys, "No matching slot found for token!");

	UNLOCK_MUTEX(slot_mutex);
}

/*
 * This function is called by the dispatch system and will call
 * scan_certificates() and build_cert_objects() and the appropriate
 * memory barrier functions
 */

static void
background_cert_scan(void *dummy)
{
	scan_certificates();
	build_cert_objects();

	atomic_store(&cert_list_status, initialized);
}

/*
 * Scan the Keychain for certificates and add them to our object database
 *
 * This uses SecItemCopyMatching, but searches for certificates that
 * have a subject name that matches one of our match strings.  Once we
 * find that certificate then we chase down all certificates issued
 * by that certificate; this means you should only need to list Root CAs
 * in your match string list.
 */

static void
scan_certificates(void)
{
	char **certs = NULL, **p;
	CFMutableArrayRef cmatch = NULL;
	CFMutableSetRef certset = NULL;
	CFMutableDictionaryRef query = NULL;
	CFTypeRef result = NULL;
	OSStatus ret;
	unsigned int i, count;
	struct certlist *cl;

	/*
	 * I tried, at first, to use the built-in searching features
	 * available in SecItemCopyMatching(), but that turned out to be
	 * a failure for two reasons:
	 *
	 * Matching on the subject name (using kSecMatchSubjectContains)
	 * SORT-of worked, except if you have a hardware token you will
	 * get certificates on the hardware token included in the list
	 * EVEN THOUGH the subject names don't match.
	 *
	 * Matching based on issuer (using kSecMatchIssuers) only works
	 * for IDENTITIES, for some strange reason (really, there is no
	 * good reason for this that I can tell).
	 *
	 * Because SecItemCopyMatching is kind of expensive, what I finally
	 * decided on was this:
	 *
	 * Get a list of ALL certificates.
	 *
	 * Generate a CFMutableSet from the original certificate array.
	 *
	 * As we add each certificate, remove it from the CFSet
	 * (to improve on later searching).
	 *
	 * Sigh.  Apple, why did you have to make this so hard?
	 */

	/*
	 * We need to retrieve all valid certificates from our Keychains.
	 *
	 * Our query dictionary:
	 *
	 * kSecClass = kSecClassCertificate
	 *	This means we're searching for certificates only,
	 *	and we don't need private key objects
	 * kSecMatchLimit = kSecMatchLimitAll
	 *	Return all matching certificates
	 * kSecMatchTrustedOnly = kCFBooleanTrue
	 *	Only match trusted certificates
	 * kSecReturnRef = kCFBooleanTrue
	 *	This means return a reference to the certificate
	 *	object (a SecCertificateRef).  Because we also use
	 *	kSecReturnAttributes that means the certificate
	 *	reference ends up in the attribute dictionary.
	 * kSecReturnAttributes = kCFBooleanTrue
	 *	This means we return all of the attributes for each 
	 *	certificate.
	 */

	/*
	 * Short circuit the search if "none" is the first entry
	 */

	certs = prefkey_arrayget("certificateList", default_cert_search);

	if (certs[0] && strcasecmp(certs[0], "none") == 0) {
		os_log_debug(logsys, "Special entry \"none\" found, not "
			     "importing Keychain certificates");
		goto out;
	}

	cmatch = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	if (! cmatch) {
		os_log_debug(logsys, "Unable to create match array!");
		goto out;
	}

	for (p = certs; *p != NULL; p++) {
		CFStringRef cm = CFStringCreateWithCString(NULL, *p,
						   kCFStringEncodingUTF8);
		CFArrayAppendValue(cmatch, cm);
		CFRelease(cm);
	}

	/*
	 * This shouldn't be 0, but JUST in case ...
	 */

	if (CFArrayGetCount(cmatch) == 0) {
		os_log_debug(logsys, "No certificate match strings found, "
			     "not importing certificates");
		goto out;
	}

	/*
	 * Get a list of all certificates
	 */

	if (! add_dict(&query, kSecClass, kSecClassCertificate))
		goto out;
	add_dict(&query, kSecMatchLimit, kSecMatchLimitAll);
	add_dict(&query, kSecMatchTrustedOnly, kCFBooleanTrue);
	add_dict(&query, kSecReturnRef, kCFBooleanTrue);
	add_dict(&query, kSecReturnAttributes, kCFBooleanTrue);

	os_log_debug(logsys, "About to call SecItemCopyMatching");

	ret = SecItemCopyMatching(query, &result);

	os_log_debug(logsys, "SecItemCopyMatching finished");

	/*
	 * If we didn't find ANY certificates at all (really? None?)
	 * then return.
	 */

	if (ret) {
		if (ret == errSecItemNotFound) {
			os_log_debug(logsys, "No valid certificates found; "
				     "that doesn't seem right!");
			goto out;
		} else {
			LOG_SEC_ERR("Certificate SecItemCopyMatching "
				    "failed: %{public}@", ret);
			goto out;
		}
	}

	count = cflistcount(result);

	os_log_debug(logsys, "Searching %u certificates", count);
	
	/*
	 * Before we do anything else, add all of the certificates
	 * entries to our set.
	 */

	certset = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);

	if (! certset) {
		os_log_debug(logsys, "Unable to create certificate set!");
		goto out;
	}

	for (i = 0; i < count; i++)
		CFSetAddValue(certset, cfgetindex(result, i));

	/*
	 * Search all of our certificate for matches, and add the
	 * results.
	 */

	cl = search_certs(certset, cmatch, NULL);

	if (! cl) {
		os_log_debug(logsys, "No matching certificates found");
	} else {
		struct certlist *c;

		for (c = cl; c != NULL; c = c->next)
			add_certificate(c->certdict, certset);

		free_certlist(cl);
	}

	os_log_debug(logsys, "%u certificates added", cert_list_count);

out:
	if (certs)
		array_free(certs);
	if (cmatch)
		CFRelease(cmatch);
	if (certset)
		CFRelease(certset);
	if (query)
		CFRelease(query);
	if (result)
		CFRelease(result);
}

/*
 * Free our cert_list
 */

static void
cert_list_free(void)
{
	unsigned int i;

	for (i = 0; i < cert_list_count; i++) {
		if (cert_list[i].cert)
			CFRelease(cert_list[i].cert);
		if (cert_list[i].pkeyhash)
			CFRelease(cert_list[i].pkeyhash);
	}

	free(cert_list);

	cert_list = NULL;

	atomic_store(&cert_list_status, uninitialized);
}

/*
 * Search our set of certificates, either based on the common name
 * or the issuer.  If we get passed in cnmatch then match based on
 * a substring search of the common names, otherwise match on an
 * identical issuer.
 *
 * We return a pointer to the head of a certlist.
 */

static struct certlist *
search_certs(CFMutableSetRef certs, CFArrayRef cnmatch, CFDataRef issuer)
{
	struct certcontext cc;
	CFSetApplierFunction fn;

	cc.head = NULL;
	cc.tail = NULL;

	if (cnmatch) {
		fn = &cn_match;
		cc.match = cnmatch;
	} else if (issuer) {
		fn = &issuer_match;
		cc.match = issuer;
	} else {
		os_log_debug(logsys, "Internal error: cnmatch and issuer "
			     "are both NULL");
		return NULL;
	}

	CFSetApplyFunction(certs, fn, &cc);

	return cc.head;
}

/*
 * Our matching function when we are matching based on the certificate
 * common name.
 */

static void
cn_match(const void *value, void *context)
{
	struct certcontext *cc = (struct certcontext *) context;
	CFDictionaryRef dict = (CFDictionaryRef) value;
	CFArrayRef cnmatch = (CFArrayRef) cc->match;
	CFStringRef cn = NULL;
	SecCertificateRef cert;
	unsigned int i, count;
	OSStatus ret;

	/*
	 * Extract out our common name from the certificate.  Get the
	 * certificate ref and then use SecCertificateCopyCommonName()
	 */

	if (! CFDictionaryGetValueIfPresent(dict, kSecValueRef,
					    (const void **) &cert)) {
		os_log_debug(logsys, "Warning: unable to retrieve certificate "
			     "from dictionary");
		return;
	}

	ret = SecCertificateCopyCommonName(cert, &cn);

	if (ret) {
		LOG_SEC_ERR("CopyCommonName failed: %{public}@", ret);
		return;
	}

	if (! cn) {
		os_log_debug(logsys, "SecCertificateCopyCommonName "
			     "returned NULL");
		return;
	}

	count = CFArrayGetCount(cnmatch);

	for (i = 0; i < count; i++) {
		CFStringRef str = CFArrayGetValueAtIndex(cnmatch, i);
		CFRange range;

		range = CFStringFind(cn, str, 0);

		if (range.length > 0) {
			/*
			 * We have a match!
			 */
			add_cert_to_list(dict, cc);
			break;
		}
	}

	CFRelease(cn);

	return;
}

/*
 * Match a certificate based on the issuer, and add it to our linked list
 * if it matches.
 */

static void
issuer_match(const void *value, void *context)
{
	struct certcontext *cc = (struct certcontext *) context;
	CFDictionaryRef dict = (CFDictionaryRef) value;
	CFDataRef match_issuer = (CFDataRef) cc->match;
	CFDataRef issuer;

	/*
	 * Get the issuer out of our certificate dictionary
	 */

	if (! CFDictionaryGetValueIfPresent(dict, kSecAttrIssuer,
					    (const void **) &issuer)) {
		os_log_debug(logsys, "Warning: cannot retrieve issuer from "
			     "certificate");
		return;
	}

	if (CFEqual(issuer, match_issuer))
		add_cert_to_list(dict, cc);

}

/*
 * Add a certificate dictionary to our linked list.
 *
 * Note that to prevent the dictionary from getting reclaimed underneath
 * us, we CFRetain() it; that means when you free the linked list you need
 * to release those objects.
 */

static void
add_cert_to_list(CFDictionaryRef dict, struct certcontext *cc)
{
	struct certlist *cl = malloc(sizeof(*cl));

	cl->certdict = dict;
	CFRetain(cl->certdict);
	cl->next = NULL;

	if (! cc->head) {
		cc->head = cc->tail = cl;
	} else {
		cc->tail->next = cl;
		cc->tail = cl;
	}
}

/*
 * Free a certificate list
 */

static void
free_certlist(struct certlist *cl)
{
	struct certlist *cl2;

	while (cl != NULL) {
		cl2 = cl->next;
		CFRelease(cl->certdict);
		free(cl);
		cl = cl2;
	}
}

/*
 * Add a certificate to our internal list that ends up on the list of
 * trusted certificates we present from our certificate slot.
 */

static void
add_certificate(CFDictionaryRef dict, CFMutableSetRef certs)
{
	SecCertificateRef cert;
	CFStringRef val;
	CFDataRef pkey, subject;
	struct certlist *cl, *ce;
	unsigned int i, c = cert_list_count;

#if 0
	if (os_log_debug_enabled(logsys)) {
		CFStringRef label = CFDictionaryGetValue(dict, kSecAttrLabel);
		os_log_debug(logsys, "Adding certificate \"%{public}@\"",
			     label ? label : CFSTR("Unknown certificate"));
	}
#endif

	/*
	 * Before we do anything else, remove us from the certificate
	 * set so we don't try to match on us again.
	 */

	CFSetRemoveValue(certs, dict);

	/*
	 * We never want hardware tokens in this list
	 */

	if (CFDictionaryGetValueIfPresent(dict, kSecAttrAccessGroup,
					  (const void **) &val)) {
		if (CFEqual(val, kSecAttrAccessGroupToken)) {
			os_log_debug(logsys, "Certificate is on hardware "
				     "token, skipping");
			return;
		}
	}

	/*
	 * Extract our the certificate reference and public key hash; use
	 * the public key hash to see if we have it already
	 */

	if (! CFDictionaryGetValueIfPresent(dict, kSecValueRef,
					    (const void **) &cert)) {
		os_log_debug(logsys, "No certificate reference found, "
			     "skipping!");
		return;
	}

	if (! CFDictionaryGetValueIfPresent(dict, kSecAttrPublicKeyHash,
					   (const void **) &pkey)) {
		os_log_debug(logsys, "No public key hash found, skipping!");
		return;
	}

	/*
	 * Search to see if we have this already.  I realize this will
	 * start to perform poorly if we get a lot of certificates, but
	 * we only do this once.
	 */

	for (i = 0; i < cert_list_count; i++) {
		if (CFEqual(pkey, cert_list[i].pkeyhash)) {
			os_log_debug(logsys, "Certificate is already in list, "
				     "skipping");
			return;
		}
	}

	/*
	 * Add this to our certificate list.
	 */

	if (++cert_list_count > cert_list_size) {
		cert_list_size += 5;

		cert_list = realloc(cert_list,
				    sizeof(*cert_list) * cert_list_size);
	}

	cert_list[c].cert = cert;
	CFRetain(cert_list[c].cert);
	cert_list[c].pkeyhash = pkey;
	CFRetain(cert_list[c].pkeyhash);

	/*
	 * Generate a list of certificates ISSUED by this certificate,
	 * and add them.
	 */

	if (! CFDictionaryGetValueIfPresent(dict, kSecAttrSubject,
					    (const void **) &subject)) {
		os_log_debug(logsys, "Unable to retrieve subject, returning");
		return;
	}

	cl = search_certs(certs, NULL, subject);

	for (ce = cl; ce != NULL; ce = ce->next)
		add_certificate(ce->certdict, certs);

	free_certlist(cl);

	return;
}

/*
 * Get the SecAccessControl object from the identity.
 *
 * This is a little subtle; we don't actually want the SecAccessControl
 * object for the IDENTITY, we want it for the private key (as it turns
 * out, they are different).  So what we need to do is get the attributes
 * for the key using SecItemCopyMatching(); the SecAccessControl object
 * will be in there.
 */

static SecAccessControlRef
getaccesscontrol(CFDictionaryRef dict)
{
	SecAccessControlRef accret;
	CFMutableDictionaryRef accquery = NULL;
	CFDictionaryRef attrdict;
	CFDataRef label;
	OSStatus ret;

	/*
	 * Our keys for our query dictionary for SecItemCopyMaching().
	 *
	 * kSecClass = kSecClassKey
	 *	This means we're searching for keys (instead of identities
	 *	or certificates)
	 * kSecAttrKeyClass = kSecAttrKeyClassPrivate
	 *	We want to match on the private key
	 * kSecAttrApplicationLabel
	 *	This is the "application label" of the key.  We get this
	 *	from the identity dictionary, and it makes sure we get the
	 *	private key associated with this identity.
	 * kSecMatchLimit = kSecMatchLimitOne
	 *	We only want one match (really, we should only have one
	 *	match, but let's be safe)
	 * kSecReturnAttributes = kCFBooleanTrue
	 *	We want to get all of the attributes so we can find the
	 *	SecAccessControlRef
	 */

	/*
	 * Build our query dictionary to retrieve the key attributes.  We
	 * need the application label from the original identity (this is
	 * passed down in "dict")
	 */

	if (! CFDictionaryGetValueIfPresent(dict, kSecAttrApplicationLabel,
				    (const void **) &label)) {
		os_log_debug(logsys, "Application Label object not found");
		return NULL;
	}

	if (! add_dict(&accquery, kSecClass, kSecClassKey))
		return NULL;
	add_dict(&accquery, kSecAttrKeyClass, kSecAttrKeyClassPrivate);
	add_dict(&accquery, kSecAttrApplicationLabel, label);
	add_dict(&accquery, kSecMatchLimit, kSecMatchLimitOne);
	add_dict(&accquery, kSecReturnAttributes, kCFBooleanTrue);

	/*
	 * Perform the actual query
	 */

	ret = SecItemCopyMatching(accquery, (CFTypeRef *) &attrdict);

	CFRelease(accquery);

	if (ret) {
		LOG_SEC_ERR("Access control ref SecItemCopyMatching "
			    "failed: %{public}@", ret);
		return NULL;
	}

	/*
	 * Just in case, make sure we got a CFDictionaryRef
	 */

	if (CFGetTypeID(attrdict) != CFDictionaryGetTypeID()) {
		logtype("Was expecting a CFDictionary, but got", attrdict);
		CFRelease(attrdict);
		return NULL;
	}

	if (! CFDictionaryGetValueIfPresent(attrdict, kSecAttrAccessControl,
					    (const void **) &accret)) {
		os_log_debug(logsys, "Access Control object not found");
		CFRelease(attrdict);
		return NULL;
	}

	CFRetain(accret);
	CFRelease(attrdict);

	return accret;
}

/*
 * Return the user-printable label for a key.
 *
 * Sigh.  It turns out some applications REALLY want a printable label
 * associated with a key; we'll fetch the key label from the attribute
 * dictionary.  Returns string that must always be free()d.
 */

static char *
getkeylabel(SecKeyRef key)
{
	CFMutableDictionaryRef query = NULL;
	CFDictionaryRef keyattr = NULL, result = NULL;
	CFStringRef label, class;
	CFDataRef applabel;
	OSStatus ret;
	char *retstr;

	/*
	 * Slightly more complicated than I would like, but we're trying to
	 * conform to the Security framework APIs as I understand them.
	 *
	 * Fetch the key attributes using SecKeyCopyAttributes (the label
	 * isn't one of the supported attributes that SecKeyCopyAttributes
	 * is supposed to return).
	 *
	 * Use the KeyClass and application label in a query dictionary
	 * to retrieve the complete attribute dictionary, and return the
	 * kSecAttrLabel value.
	 */

	/*
	 * Our query dictionary; see above for greater detail
	 *
	 * kSecClass = kSecClassKey
	 * kSecAttrKeyClass (from key)
	 * kSecAttrApplicationLabel (from key)
	 * kSecMatchLimit = kSecMatchLimitOne
	 * kSecReturnAttributes = kCFBooleanTrue
	 */

	keyattr = SecKeyCopyAttributes(key);

	if (! keyattr) {
		os_log_debug(logsys, "SecKeyCopyAttr returned NULL");
		retstr = strdup("Unknown key");
		goto out;
	}

	if (! CFDictionaryGetValueIfPresent(keyattr, kSecAttrKeyClass,
					    (const void **) &class)) {
		os_log_debug(logsys, "Cannot find KeyClass in dict");
		retstr = strdup("Unknown key");
		goto out;
	}

	if (! CFDictionaryGetValueIfPresent(keyattr, kSecAttrApplicationLabel,
					    (const void **) &applabel)) {
		os_log_debug(logsys, "Cannot find AppLabel in dict");
		retstr = strdup("Unknown key");
		goto out;
	}

	if (! add_dict(&query, kSecClass, kSecClassKey)) {
		retstr = strdup("Unknown key");
		goto out;
	}
	add_dict(&query, kSecAttrKeyClass, class);
	add_dict(&query, kSecAttrApplicationLabel, applabel);
	add_dict(&query, kSecMatchLimit, kSecMatchLimitOne);
	add_dict(&query, kSecReturnAttributes, kCFBooleanTrue);

	ret = SecItemCopyMatching(query, (CFTypeRef *) &result);

	if (ret) {
		LOG_SEC_ERR("SecItemCopyMatching failed: %{public}@", ret);
		retstr = strdup("Unknown key");
		goto out;
	}

	if (CFGetTypeID(result) != CFDictionaryGetTypeID()) {
		logtype("Was expecting a CFDictionaryRef, but got", result);
		retstr = strdup("Unknown key");
		goto out;
	}

	if (! CFDictionaryGetValueIfPresent(result, kSecAttrLabel,
					    (const void **) &label)) {
		os_log_debug(logsys, "Unable to find key label");
		retstr = strdup("Unknown key");
	} else {
		retstr = getstrcopy(label);
	}

out:
	if (keyattr)
		CFRelease(keyattr);
	if (query)
		CFRelease(query);
	if (result)
		CFRelease(result);

	return retstr;
}

/*
 * A "safe" version of CFArrayGetCount().
 *
 * SecItemCopyMatching can sometimes return a single entry or a CFArrayRef
 * with multiple entries.  So to be as robust as possible, handle this case
 * here.  If we have something OTHER than a CFArrayRef, then just return a
 * count of "1".  Otherwise return the real array count.
 */

static unsigned int
cflistcount(CFTypeRef ref)
{
	if (CFGetTypeID(ref) == CFArrayGetTypeID()) {
		return CFArrayGetCount((CFArrayRef) ref);
	} else {
		return 1;
	}
}

/*
 * A "safe" version of CFArrayGetValueAtIndex
 *
 * If the passed-in type is a CFArray, then return the appropriate value
 * at the passed-in index.  If it is a CFDictionaryRef, then just return
 * passed-in value if the index is 0; anything else, return a NULL.
 */

static CFDictionaryRef
cfgetindex(CFTypeRef ref, unsigned int index)
{
	if (CFGetTypeID(ref) == CFArrayGetTypeID()) {
		return CFArrayGetValueAtIndex((CFArrayRef) ref, index);
	} else if (CFGetTypeID(ref) == CFDictionaryGetTypeID() && index == 0) {
		return ref;
	} else {
		return NULL;
	}
}

/*
 * Make sure the our custom logging system is enabled
 */

static void
log_init(void *context)
{
	logsys = os_log_create(APPIDENTIFIER, "general");
}

/*
 * Free a slot entry
 */

static void
slot_entry_free(struct slot_entry *entry, bool logout)
{
	os_log_debug(logsys, "slot_entry_free for slot %{public}@ "
		    "(refcount %d)", entry->tokenid, (int) entry->refcount);

	LOCK_MUTEX(entry->entry_mutex);

	if (--entry->refcount > 0) {
		/*
		 * If refcount == 1, that means there are no more open
                 * sessions (assuming we didn't have an open session and
                 * our card got removed).  So if logout == true (called
                 * be C_CloseSession or similar functions) then logout
                 * of the token as well.
		 *
		 * If the refcount == 0 then the lacontext is completety
		 * released which does the same thing.
		 */
		if (entry->refcount == 1)
			token_logout(entry);
		UNLOCK_MUTEX(entry->entry_mutex);
		return;
	}

	if (entry->tokenid)
		CFRelease(entry->tokenid);

	if (entry->id_list)
		id_list_free(entry->id_list, entry->id_count);

	if (entry->label)
		free(entry->label);

	if (entry->lacontext)
		lacontext_free(entry->lacontext);

	UNLOCK_MUTEX(entry->entry_mutex);
	DESTROY_MUTEX(entry->entry_mutex);

	free(entry);
}

/*
 * Free our identity list
 */

static void
id_list_free(struct id_info **id_list, unsigned int id_list_count)
{
	int i;

	for (i = 0; i < id_list_count; i++)
		if (id_list[i])
			id_info_free(id_list[i]);

	if (id_list)
		free(id_list);
}

/*
 * Free a single identity entry
 */

static void
id_info_free(struct id_info *id)
{
	if (id->label)
		free(id->label);
	if (id->ident)
		CFRelease(id->ident);
	if (id->privkey)
		CFRelease(id->privkey);
	if (id->pubkey)
		CFRelease(id->pubkey);
	if (id->cert)
		CFRelease(id->cert);
	if (id->secaccess)
		CFRelease(id->secaccess);
	if (id->pkeyhash)
		CFRelease(id->pkeyhash);

	free(id);
}

/*
 * A version of snprintf() which does space-padding
 */

static void
sprintfpad(unsigned char *dest, size_t destsize, const char *fmt, ...)
{
	char *s;
	va_list ap;
	int rc;

	va_start(ap, fmt);
	rc = vasprintf(&s, fmt, ap);
	va_end(ap);

	if (rc < 1) {
		memset(dest, ' ', destsize);
	} else {
		/* We are relying on strncpy not doing \0 at end for trunc */
		strncpy((char *) dest, s, destsize);
		if (rc < destsize)
			memset(dest + rc, ' ', destsize - rc);
		free(s);
	}
}

/*
 * Return a boolean value based on a dictionary key.  If the key is not
 * set then return false.
 */

bool
boolfromdict(const char *keyname, CFDictionaryRef dict, CFTypeRef key)
{
	CFTypeRef val;

	if (! CFDictionaryGetValueIfPresent(dict, key, &val)) {
		os_log_debug(logsys, "No value for %s in dictionary, "
			     "returning FALSE", keyname);
		return false;
	}

	if (CFGetTypeID(val) != CFBooleanGetTypeID() &&
	    CFGetTypeID(val) != CFNumberGetTypeID()) {
		os_log_debug(logsys, "%s was not a boolean, but exists, so "
			     "returning TRUE", keyname);
		return true;
	}

	os_log_debug(logsys, "%s is set to %{bool}d", keyname,
		     CFBooleanGetValue(val));

	return CFBooleanGetValue(val);
}

/*
 * Get a C string from a CFStringRef (assumes UTF-8 encoding).
 * Allocates memory that must be free()d.
 */

static char *
getstrcopy(CFStringRef string)
{
	const char *s = CFStringGetCStringPtr(string, kCFStringEncodingUTF8);

	if (! s) {
		CFIndex len = CFStringGetLength(string);
		CFIndex size = CFStringGetMaximumSizeForEncoding(len,
						 kCFStringEncodingUTF8) + 1;
		char *p = malloc(size);

		if (! CFStringGetCString(string, p, size,
					 kCFStringEncodingUTF8)) {
			free(p);
			return strdup("Unknown string");
		}

		return p;
	} else {
		return strdup(s);
	}
}

/*
 * Log this object's type
 */

void
logtype(const char *string, CFTypeRef ref)
{
	CFTypeID id = CFGetTypeID(ref);
	CFStringRef str = CFCopyTypeIDDescription(id);

	os_log_debug(logsys, "%s: %{public}@", string, str);

	CFRelease(str);
}

#if defined(KEYCHAIN_DEBUG)
/*
 * Dump the contents of a dictionary
 */

void
dumpdict(const char *string, CFDictionaryRef dict)
{
	unsigned int i, count = CFDictionaryGetCount(dict);
	const void **keys, **values;

	os_log_debug(logsys, "Dumping dictionary for %s", string);
	os_log_debug(logsys, "Dictionary contains %u key/value pairs", count);

	keys = malloc(sizeof(void *) * count);
	values = malloc(sizeof(void *) * count);

	CFDictionaryGetKeysAndValues(dict, keys, values);

	for (i = 0; i < count; i++) {
		os_log_debug(logsys, "Dictionary entry %d", (int) i);
		os_log_debug(logsys, "Key value: %{public}@", keys[i]);
		logtype("Value type", values[i]);
		os_log_debug(logsys, "Value value: %{public}@", values[i]);
	}

	free(keys);
	free(values);
}
#endif /* KEYCHAIN_DEBUG */

/*
 * Convert Security framework key types to PKCS#11 key types.
 *
 * Sigh.  This is a lot harder than I would like.  The Apple API is in flux;
 * what the attribute dictionary returns is a CFNumber, but the constants
 * you use are actually CFStrings which happen to have string values which
 * correspond to the CFNumbers.  The numbers correspond to the values of
 * CSSM_ALGORITHMS in cssmtype.h.  So that means a RSA key, for example,
 * shows up in the attribute dictionary as a CFNumber with a value of 42,
 * but the constant kSecAttrKeyTypeRSA is a CFString with the value of "42".
 * (There is magic in the Security framework that lets you use things like
 * kSecAttrKeyTypeRSA as an input key for kSecAttrKeyType).
 *
 * I am hesitant to include cssmtype.h, so what I have decided to do is
 * convert the dictionary number we are given to a string and compare it
 * against the relevant "new" API constants.  Hopefully we don't have that
 * many.
 */

static CK_KEY_TYPE
convert_keytype(CFNumberRef type)
{
	int i;
	CFStringRef str = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@"),
						   type);

	for (i = 0; keytype_map[i].keyname; i++) {
		if (CFEqual(str, *keytype_map[i].sec_keytype)) {
			os_log_debug(logsys, "This is a %s",
				     keytype_map[i].keyname);
			CFRelease(str);
			return keytype_map[i].pkcs11_keytype;
		}
	}

	CFRelease(str);

	os_log_debug(logsys, "Keytype is unknown, returning VENDOR_DEFINED");

	return CKK_VENDOR_DEFINED;
}

/*
 * Build our list of objects based on our identities
 */

#define ADD_ATTR_SIZE(objlist, objcount, attribute, var, size) \
do { \
	void *p = malloc(size); \
	memcpy(p, var, size); \
	if ( objlist [ objcount ].attr_count >= \
	    objlist [ objcount ].attr_size) { \
		objlist [ objcount ].attr_size += 5; \
		objlist [ objcount ].attrs = realloc( objlist [ objcount ].attrs, \
			objlist [ objcount ].attr_size * sizeof(CK_ATTRIBUTE)); \
	} \
	objlist [ objcount ].attrs[ objlist [ objcount ].attr_count].type = attribute; \
	objlist [ objcount ].attrs[ objlist [ objcount ].attr_count].pValue = p; \
	objlist [ objcount ].attrs[ objlist [ objcount ].attr_count].ulValueLen = size; \
	objlist [ objcount ].attr_count++; \
} while (0)

#define ADD_ATTR(objlist, objcount, attr, var) \
		ADD_ATTR_SIZE(objlist, objcount, attr, &var, sizeof(var))

#define NEW_OBJECT(objlist, objcount, objsize) \
do { \
	if (++ objcount >= objsize ) { \
		objsize += 5; \
		objlist = realloc( objlist, objsize * sizeof(* objlist )); \
	} \
} while (0)

#define OBJINIT(objlist, objcount, idptr) \
do { \
	objlist [ objcount ].id = idptr; \
	objlist [ objcount ].attrs = NULL; \
	objlist [ objcount ].attr_count = 0; \
	objlist [ objcount ].attr_size = 0; \
} while (0)

/*
 * Build up a list of objects based on our identity list
 */

static void
build_id_objects(struct slot_entry *entry)
{
	int i;
	CK_OBJECT_CLASS cl;
	CK_CERTIFICATE_TYPE ct = CKC_X_509;	/* Only this for now */
	CK_ULONG t;
	CK_BBOOL b;
	CFDataRef d;
	char *label;


	if (entry->id_count > 0) {
		/* Prime the pump */
		NEW_OBJECT(entry->obj_list, entry->obj_count, entry->obj_size);
		entry->obj_count--;
	}

	for (i = 0; i < entry->id_count; i++) {
		SecCertificateRef cert = entry->id_list[i]->cert;
		CFDataRef subject = NULL, issuer = NULL, serial = NULL;
		CFDataRef keydata = NULL, modulus = NULL, exponent = NULL;
		CFErrorRef error;
		unsigned char *objid = NULL;
		unsigned int objidlen;

		OBJINIT(entry->obj_list, entry->obj_count, entry->id_list[i]);

		/*
		 * Add in the object for each identity; cert, public key,
		 * private key.  Add in attributes we need.
		 */

		get_index_bytes(i, &objid, &objidlen);

		cl = CKO_CERTIFICATE;
		entry->obj_list[entry->obj_count].class = cl;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_CLASS, cl);
		ADD_ATTR_SIZE(entry->obj_list, entry->obj_count, CKA_ID,
			      objid, objidlen);
		ADD_ATTR(entry->obj_list, entry->obj_count,
			 CKA_CERTIFICATE_TYPE, ct);
		b = CK_TRUE;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_TOKEN, b);
		ADD_ATTR_SIZE(entry->obj_list, entry->obj_count, CKA_LABEL,
			      entry->id_list[i]->label,
			      strlen(entry->id_list[i]->label));
		d = SecCertificateCopyData(cert);
		ADD_ATTR_SIZE(entry->obj_list, entry->obj_count, CKA_VALUE,
			      CFDataGetBytePtr(d), CFDataGetLength(d));
		get_certificate_info(d, &serial, &issuer, &subject);
		CFRelease(d);

		if (subject)
			ADD_ATTR_SIZE(entry->obj_list, entry->obj_count,
				      CKA_SUBJECT, CFDataGetBytePtr(subject),
				      CFDataGetLength(subject));
		if (issuer)
			ADD_ATTR_SIZE(entry->obj_list, entry->obj_count,
				      CKA_ISSUER, CFDataGetBytePtr(issuer),
				      CFDataGetLength(issuer));
		if (serial)
			ADD_ATTR_SIZE(entry->obj_list, entry->obj_count,
				      CKA_SERIAL_NUMBER,
				      CFDataGetBytePtr(serial),
				      CFDataGetLength(serial));

		NEW_OBJECT(entry->obj_list, entry->obj_count, entry->obj_size);
		OBJINIT(entry->obj_list, entry->obj_count, entry->id_list[i]);

		cl = CKO_PUBLIC_KEY;
		entry->obj_list[entry->obj_count].class = cl;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_CLASS, cl);
		ADD_ATTR_SIZE(entry->obj_list, entry->obj_count, CKA_ID,
			      objid, objidlen);
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_KEY_TYPE,
			 entry->id_list[i]->keytype);
		b = CK_TRUE;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_TOKEN, b);
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_LOCAL, b);
		b = entry->id_list[i]->pubcanencrypt;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_ENCRYPT, b);
		b = entry->id_list[i]->pubcanverify;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_VERIFY, b);
		if (subject)
			ADD_ATTR_SIZE(entry->obj_list, entry->obj_count,
				      CKA_SUBJECT, CFDataGetBytePtr(subject),
				      CFDataGetLength(subject));

		/*
		 * Sigh.  It seems like the public part of an identity
		 * doesn't actually get a label attribute, at least with
		 * the release I tested.  So for now, get the label from
		 * the identity label, and maybe check later if this
		 * changes; keep the code around here if it does.
		 *
		 * label = getkeylabel(entry->id_list[i]->pubkey);
		 * ADD_ATTR_SIZE(id, CKA_LABEL, label, strlen(label));
		 * free(label);
		 */

		ADD_ATTR_SIZE(entry->obj_list, entry->obj_count, CKA_LABEL,
			      entry->id_list[i]->label,
			      strlen(entry->id_list[i]->label));

		/*
		 * It turns out some implementations want CKA_MODULUS_BITS,
		 * and the modulus and public exponent.  For RSA keys the
		 * modulus size is equal to the block size, and we can get
		 * modulus and public exponent from the "external
		 * representation" of the public key.  Note that the block
		 * size is returned in bytes, and we need bits.
		 */

		t = SecKeyGetBlockSize(entry->id_list[i]->pubkey) * 8;
		ADD_ATTR(entry->obj_list, entry->obj_count,
			 CKA_MODULUS_BITS, t);

		keydata = SecKeyCopyExternalRepresentation(entry->id_list[i]->pubkey,
							   &error);

		if (keydata) {
			if (get_pubkey_info(keydata, &modulus, &exponent)) {
				ADD_ATTR_SIZE(entry->obj_list,
					      entry->obj_count, CKA_MODULUS,
					      CFDataGetBytePtr(modulus),
					      CFDataGetLength(modulus));
				ADD_ATTR_SIZE(entry->obj_list,
					      entry->obj_count,
					      CKA_PUBLIC_EXPONENT,
					      CFDataGetBytePtr(exponent),
					      CFDataGetLength(exponent));
			}
		} else {
			os_log_debug(logsys, "SecKeyCopyExternalRepresentation "
				     "failed: %{public}@", error);
			CFRelease(error);
		}

		b = CK_FALSE;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_WRAP, b);
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_DERIVE, b);

		NEW_OBJECT(entry->obj_list, entry->obj_count, entry->obj_size);
		OBJINIT(entry->obj_list, entry->obj_count, entry->id_list[i]);

		cl = CKO_PRIVATE_KEY;
		entry->obj_list[entry->obj_count].class = cl;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_CLASS, cl);
		ADD_ATTR_SIZE(entry->obj_list, entry->obj_count, CKA_ID,
			      objid, objidlen);
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_KEY_TYPE,
			 entry->id_list[i]->keytype);
		b = CK_TRUE;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_TOKEN, b);
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_PRIVATE, b);
		b = entry->id_list[i]->privcandecrypt;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_DECRYPT, b);
		b = entry->id_list[i]->privcansign;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_SIGN, b);
		if (subject)
			ADD_ATTR_SIZE(entry->obj_list, entry->obj_count,
				      CKA_SUBJECT, CFDataGetBytePtr(subject),
				      CFDataGetLength(subject));

		label = getkeylabel(entry->id_list[i]->privkey);
		ADD_ATTR_SIZE(entry->obj_list, entry->obj_count, CKA_LABEL,
			      label, strlen(label));
		free(label);

		/*
		 * I guess some applications want the modulus and public
		 * exponent as attributes in the private key object.
		 * Use this information we extracted previously.
		 */

		if (keydata) {
			if (modulus)
				ADD_ATTR_SIZE(entry->obj_list, entry->obj_count,
						CKA_MODULUS, CFDataGetBytePtr(modulus),
						CFDataGetLength(modulus));
			if (exponent)
				ADD_ATTR_SIZE(entry->obj_list, entry->obj_count,
						CKA_PUBLIC_EXPONENT,
						CFDataGetBytePtr(exponent),
						CFDataGetLength(exponent));
		}

		b = CK_TRUE;
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_SENSITIVE, b);
		ADD_ATTR(entry->obj_list, entry->obj_count,
			 CKA_ALWAYS_SENSITIVE, b);
		ADD_ATTR(entry->obj_list, entry->obj_count,
			 CKA_NEVER_EXTRACTABLE, b);
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_LOCAL, b);
		b = CK_FALSE;
		ADD_ATTR(entry->obj_list, entry->obj_count,
			 CKA_ALWAYS_AUTHENTICATE, b);
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_UNWRAP, b);
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_DERIVE, b);
		ADD_ATTR(entry->obj_list, entry->obj_count, CKA_EXTRACTABLE, b);

		NEW_OBJECT(entry->obj_list, entry->obj_count, entry->obj_size);

		if (objid)
			free(objid);

		if (subject)
			CFRelease(subject);
		if (issuer)
			CFRelease(issuer);
		if (serial)
			CFRelease(serial);

		if (keydata)
			CFRelease(keydata);
		if (modulus)
			CFRelease(modulus);
		if (exponent)
			CFRelease(exponent);
	}
}

/*
 * Build up a list of certificate objects
 */

static void
build_cert_objects(void)
{
	int i;
	CK_OBJECT_CLASS cl;
	CK_CERTIFICATE_TYPE ct = CKC_X_509;	/* Only this for now */
	CK_TRUST trust = CKT_NSS_TRUSTED_DELEGATOR;
	CK_BBOOL b;
	CFDataRef d;

	if (cert_list_count > 0) {
		/* Prime the pump */
		NEW_OBJECT(cert_obj_list, cert_obj_count, cert_obj_size);
		cert_obj_count--;
	}

	for (i = 0; i < cert_list_count; i++) {
		SecCertificateRef cert = cert_list[i].cert;
		CFDataRef subject = NULL, issuer = NULL, serial = NULL;
		unsigned char *hash = NULL;
		unsigned int hashlen;
		md_context mdc;
		CFStringRef subjstr;
		char *subjc;
		unsigned char *objid = NULL;
		unsigned int objidlen;

		OBJINIT(cert_obj_list, cert_obj_count, NULL);

		/*
		 * Add in an object for each certificate.
		 */

		get_index_bytes(i, &objid, &objidlen);

		cl = CKO_CERTIFICATE;
		cert_obj_list[cert_obj_count].class = cl;
		ADD_ATTR(cert_obj_list, cert_obj_count, CKA_CLASS, cl);
		ADD_ATTR_SIZE(cert_obj_list, cert_obj_count, CKA_ID, objid,
			      objidlen);
		ADD_ATTR(cert_obj_list, cert_obj_count,
			 CKA_CERTIFICATE_TYPE, ct);
		b = CK_TRUE;
		ADD_ATTR(cert_obj_list, cert_obj_count, CKA_TOKEN, b);

		subjstr = SecCertificateCopySubjectSummary(cert_list[i].cert);
		subjc = getstrcopy(subjstr);

		ADD_ATTR_SIZE(cert_obj_list, cert_obj_count, CKA_LABEL,
			      subjc, strlen(subjc));

		free(subjc);
		CFRelease(subjstr);

		d = SecCertificateCopyData(cert);
		ADD_ATTR_SIZE(cert_obj_list, cert_obj_count, CKA_VALUE,
			      CFDataGetBytePtr(d), CFDataGetLength(d));
		get_certificate_info(d, &serial, &issuer, &subject);

		if (cc_md_init(CKM_SHA_1, &mdc)) {
			cc_md_update(mdc, CFDataGetBytePtr(d),
				     CFDataGetLength(d));
			cc_md_final(mdc, &hash, &hashlen);
		}

		CFRelease(d);

		if (subject)
			ADD_ATTR_SIZE(cert_obj_list, cert_obj_count,
				      CKA_SUBJECT, CFDataGetBytePtr(subject),
				      CFDataGetLength(subject));
		if (issuer)
			ADD_ATTR_SIZE(cert_obj_list, cert_obj_count, CKA_ISSUER,
				      CFDataGetBytePtr(issuer),
				      CFDataGetLength(issuer));
		if (serial)
			ADD_ATTR_SIZE(cert_obj_list, cert_obj_count,
				      CKA_SERIAL_NUMBER,
				      CFDataGetBytePtr(serial),
				      CFDataGetLength(serial));

		NEW_OBJECT(cert_obj_list, cert_obj_count, cert_obj_size);
		OBJINIT(cert_obj_list, cert_obj_count, NULL);

		cl = CKO_NSS_TRUST;
		cert_obj_list[cert_obj_count].class = cl;
		ADD_ATTR(cert_obj_list, cert_obj_count, CKA_CLASS, cl);
		b = CK_TRUE;
		ADD_ATTR(cert_obj_list, cert_obj_count, CKA_TOKEN, b);

		if (issuer)
			ADD_ATTR_SIZE(cert_obj_list, cert_obj_count, CKA_ISSUER,
				      CFDataGetBytePtr(issuer),
				      CFDataGetLength(issuer));
		if (serial)
			ADD_ATTR_SIZE(cert_obj_list, cert_obj_count,
				      CKA_SERIAL_NUMBER,
				      CFDataGetBytePtr(serial),
				      CFDataGetLength(serial));
		if (hash)
			ADD_ATTR_SIZE(cert_obj_list, cert_obj_count,
				      CKA_CERT_SHA1_HASH, hash, hashlen);

		/*
		 * As far as I can tell, CAs should have these various
		 * trust objects set, but other certificates (servers,
		 * users) should NOT.
		 */

		if (is_cert_ca(cert)) {
			ADD_ATTR(cert_obj_list, cert_obj_count,
				 CKA_TRUST_SERVER_AUTH, trust);
			ADD_ATTR(cert_obj_list, cert_obj_count,
				 CKA_TRUST_CLIENT_AUTH, trust);
			ADD_ATTR(cert_obj_list, cert_obj_count,
				 CKA_TRUST_EMAIL_PROTECTION, trust);
			ADD_ATTR(cert_obj_list, cert_obj_count,
				 CKA_TRUST_CODE_SIGNING, trust);
#if 0
			ADD_ATTR(cert_obj_list, cert_obj_count,
				 CKA_TRUST_STEP_UP_APPROVED, trust);
#endif
		}

		NEW_OBJECT(cert_obj_list, cert_obj_count, cert_obj_size);

		if (objid)
			free(objid);

		if (subject)
			CFRelease(subject);
		if (issuer)
			CFRelease(issuer);
		if (serial)
			CFRelease(serial);
		if (hash)
			free(hash);
	}
}

/*
 * Free our object list and all associated data
 */

static void
obj_free(struct obj_info **obj, unsigned int *count, unsigned int *size)
{
	int i, j;

	for (i = 0; i < *count; i++) {
		for (j = 0; j < (*obj)[i].attr_count; j++)
			free((*obj)[i].attrs[j].pValue);
		free((*obj)[i].attrs);
	}

	free(*obj);

	*obj = NULL;
	*count = *size = 0;
}

/*
 * Search an object to see if our attributes match.  If we have no
 * attributes then that counts as a match.
 */

static bool
search_object(struct obj_info *obj, CK_ATTRIBUTE_PTR attrs,
	      unsigned int attrcount)
{
	int i, j;

	/*
	 * If we get a valid "hit", then goto next to continue the
	 * attrs loop; if we make to the end of the attrs loop then
	 * we can return 'true'
	 */

	for (i = 0; i < attrcount; i++) {
		for (j = 0; j < obj->attr_count; j++)
			/*
			 * For a match, the type has to be the same, both
			 * have to have the same length, and either both
			 * are NULL pointers or both have the same contents
			 */
			if (obj->attrs[j].type == attrs[i].type &&
			    obj->attrs[j].ulValueLen == attrs[i].ulValueLen) {
			    	/*
				 * We are assuming that we only have one
				 * copy of an attribute in an object.  So
				 * if the attribute doesn't match then
				 * we can short-circuit the match now
				 */
				if ((obj->attrs[j].pValue == NULL ||
				     attrs[i].pValue == NULL) &&
				    (obj->attrs[j].pValue != attrs[i].pValue))
					return false;

				/*
				 * Both are valid pointers and have the same
				 * length, so do a memcmp().  But again, if
				 * doesn't match than return false.
				 */
				if (memcmp(obj->attrs[j].pValue,
					   attrs[i].pValue,
					   attrs[i].ulValueLen) == 0)
					goto next;
				else
					return false;
			}

		/*
		 * If we made it here then that means we went through
		 * every attribute in this object and didn't find a match
		 * so we can return false now.
		 */

		return false;
next:	;
	}

	return true;
}

/*
 * Search an object for a particular attribute; return NULL if not found
 */

static CK_ATTRIBUTE_PTR
find_attribute(struct obj_info *obj, CK_ATTRIBUTE_TYPE type)
{
	int i;

	for (i = 0; i < obj->attr_count; i++)
		if (obj->attrs[i].type == type)
			return &obj->attrs[i];

	return NULL;
}

/*
 * Output information about an attribute
 */

static void
dump_attribute(const char *str, CK_ATTRIBUTE_PTR attr)
{
	char *cn;

	if (!os_log_debug_enabled(logsys))
		return;

	switch (attr->type) {
	case CKA_CLASS:
		os_log_debug(logsys, "%s: CKA_CLASS: %s", str,
			     getCKOName(*((CK_OBJECT_CLASS *) attr->pValue)));
		break;
	case CKA_SUBJECT:
	case CKA_ISSUER:
		cn = get_common_name(attr->pValue, attr->ulValueLen);
		os_log_debug(logsys, "%s: %s: %{public}s", str,
			     getCKAName(attr->type), cn);
		free(cn);
		break;
	case CKA_TOKEN:
		os_log_debug(logsys, "%s: %s: %{bool}d", str,
			     getCKAName(attr->type),
			     (int) ((unsigned char *) attr->pValue)[0]);
		break;
	default:
		os_log_debug(logsys, "%s: %s, len = %lu, val = %p", str,
			     getCKAName(attr->type), attr->ulValueLen,
					attr->pValue);
	}
}

/*
 * Fetch a preferences key from our dictionary.  If not found, return a
 * default-provided list.  If there are no defaults, return NULL.
 *
 * Returns storage that must always be free()d.
 */

static char **
prefkey_arrayget(const char *key, const char **default_list)
{
	CFTypeID id;
	CFPropertyListRef propref;
	CFStringRef keyref;
	char **ret;

	keyref = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);

	propref = CFPreferencesCopyAppValue(keyref, CFSTR(APPIDENTIFIER));
	CFRelease(keyref);

	if (! propref) {
		/*
		 * We didn't find any matching key.  If we have a default
		 * list then return the copy of it.  If we don't, return NULL.
		 */

		unsigned int dsize = 0, i;

		if (! default_list)
			return NULL;

		while (default_list[dsize])
			dsize++;

		ret = malloc(sizeof(char *) * (dsize + 1));

		for (i = 0; i < dsize; i++)
			ret[i] = strdup(default_list[i]);

		ret[i] = NULL;

		return ret;
	}

	/*
	 * We only handle a CFStringRef or a CFArrayRef
	 */

	id = CFGetTypeID(propref);

	if (id == CFStringGetTypeID()) {
		/*
		 * Just make a two-element array and return the string
		 */

		ret = malloc(sizeof(char *) * 2);

		ret[0] = getstrcopy(propref);
		ret[1] = NULL;
	} else if (id == CFArrayGetTypeID()) {
		unsigned int i, count = CFArrayGetCount(propref);

		ret = malloc(sizeof(char *) * (count + 1));

		for (i = 0; i < count; i++)
			ret[i] = getstrcopy(CFArrayGetValueAtIndex(propref, i));

		ret[i] = NULL;
	} else {
		logtype("Unknown preference return type", propref);
		ret = NULL;
	}

	CFRelease(propref);

	return ret;
}

/*
 * See if a particular key is set in our preferences dictionary.
 *
 * It may be a single string, or an array (that's all we support right now).
 * Return true if it matches (or was found in the array).
 */

static bool
prefkey_found(const char *key, const char *value, const char **default_list)
{
	char **strlist, **p;
	bool ret = false;

	strlist = prefkey_arrayget(key, default_list);

	if (! strlist)
		return false;

	/*
	 * We are guaranteed at least one entry.  If it is "all" or "none"
	 * then do the obvious things.
	 */

	if (strcasecmp(strlist[0], "all") == 0)
		ret = true;
	else if (strcasecmp(strlist[0], "none") == 0)
		ret = false;
	else {
		/*
		 * Return "true" if we find a match
		 */

		for (p = strlist; *p != NULL; p++) {
			if (strcasecmp(*p, value) == 0) {
				ret = true;
				break;
			}
		}
	}

	array_free(strlist);

	return ret;
}

/*
 * Get a sequence of bytes to be used for an the CKA_ID attribute
 *
 * Previously I just used the value of the identity/certificate array
 * index as the raw identity bytes.  But that resulted in long identities
 * (they ended up being 8 bytes) and I got a few complaints from users
 * because in some applications you need to use the CKA_ID identity
 * directly.
 *
 * So what we're doing here is returning the shortest possible byte string
 * that identifies the index.  Encode the index in big-endian order since
 * that makes it easier for humans to deal with.  Caller will need to
 * free the returned string.
 */

static void
get_index_bytes(unsigned int index, unsigned char **retbytes,
		unsigned int *retlength)
{
	unsigned int length;
	unsigned char *bytes;
	int i;

	/*
	 * Figure out where we need to start grabbing bytes from the
	 * index.  Note we always grab the last byte even if the value of
	 * index is "0".
	 */

	for (i = sizeof(index); i > 1; i--) {
		/*
		 * Just so I remember ...
		 *
		 * if sizeof(index) == 4, then we want to shift right
		 * _3_ bytes and test if that is nonzero.  If it is,
		 * then we need to copy 4 bytes.  If it is zero, the
		 * shift right 2 bytes and if it is nonzero then copy
		 * 3 bytes, and so on ...
		 */
		if ((index >> ((i - 1) * 8)) != 0)
			break;
	}

	length = i;

	bytes = malloc(length);

	for (i = 0; i < length; i++) {
		/*
		 * Again, for later ...
		 *
		 * Counting from the left (starting at 0), we want the
		 * sizeof(index) - (length - i) byte.  If index is 4 bytes
		 * and length is 3, we want byte 1.  Then we want byte 2,
		 * then byte 3.  But to get byte N, we need to shift right
		 * by sizeof(index) - N - 1 bytes.  That works out to be
		 * length - i - 1.  Go figure.
		 */
		bytes[i] = (index >> ((length - i - 1) * 8)) & 0xff;
	}

	*retbytes = bytes;
	*retlength = length;
}

/*
 * Return a pointer to the mechanism mapping structure, which contains
 * the mappings from the Cryptoki mechanism name to the constants used by
 * the Apple Security framework
 */

const static struct mechanism_map *
get_mechmap(CK_MECHANISM_TYPE mechtype)
{
	int i;

	for (i = 0; i < keychain_mechmap_size; i++) {
		if (mechtype == keychain_mechmap[i].cki_mech)
			return &keychain_mechmap[i];
	}

	return NULL;
}

/*
 * Validate the mechanism params (if given) and map the mechanism and
 * parameters to Security framework algorithms
 */

static bool
mech_param_validate(CK_MECHANISM_PTR mptr, const struct mechanism_map *mechmap,
		    SecKeyAlgorithm *encalg, SecKeyAlgorithm *signalg,
		    SecKeyAlgorithm *dsignalg, CK_MECHANISM_TYPE *digest)
{
	CK_RSA_PKCS_OAEP_PARAMS_PTR oaep;
	CK_RSA_PKCS_PSS_PARAMS_PTR pss;
	int i;

	/*
	 * Handle the various type of mechanism parameters
	 */

	switch (mechmap->parameters) {
	case NONE:
		/*
		 * We can just return the algorithms.  But we shouldn't
		 * be taking any parameters, so return an error if we
		 * do.
		 */

		if (mptr->pParameter != NULL || mptr->ulParameterLen != 0) {
			os_log_debug(logsys, "Error: mechanism should not "
				     "take a parameter, but it looks like "
				     "we got one (pParameter = %p, len = %d)",
				     mptr->pParameter,
				     (int) mptr->ulParameterLen);
			return false;
		}

		if (encalg) {
			*encalg = mechmap->sec_encmech ?
					*mechmap->sec_encmech : NULL;
			os_log_debug(logsys, "Encryption algorithm chosen: "
				     "%{public}@", *encalg);
		}

		if (signalg) {
			*signalg = mechmap->sec_signmech ?
					*mechmap->sec_signmech : NULL;
			os_log_debug(logsys, "Signing algorithm chosen: "
				     "%{public}@", *signalg);
		}

		if (dsignalg) {
			*dsignalg = mechmap->sec_dsignmech ?
					*mechmap->sec_dsignmech : NULL;
			os_log_debug(logsys, "Digest signing algorithm chosen: "
				     "%{public}@", *dsignalg);
		}

		if (digest) {
			*digest = mechmap->sec_digest;
			os_log_debug(logsys, "Digest algorithm chosen: "
				     "%{public}s", getCKMName(*digest));
	}

		return true;

	case OAEP:
		if (mptr->pParameter == NULL ||
		    mptr->ulParameterLen != sizeof(*oaep)) {
			os_log_debug(logsys, "Error: OAEP parameter invalid "
				     "(pParameter = %p, len = %d)",
				     mptr->pParameter,
				     (int) mptr->ulParameterLen);
			return false;
		}

		oaep = (CK_RSA_PKCS_OAEP_PARAMS_PTR) mptr->pParameter;

		/*
		 * We don't support a "source" feature at all, and that
		 * doesn't seem to be used by anyone.  So we can accept
		 * two things.  A "source" of 0, or a "source" of
		 * CKZ_DATA_SPECIFIED with source ptr of NULL and length
		 * of 0.
		 */

		if (oaep->source != 0 && oaep->source != CKZ_DATA_SPECIFIED) {
			os_log_debug(logsys, "Error: invalid OAEP source: %#x",
				     (unsigned int) oaep->source);
			return false;
		}

		if (oaep->source == CKZ_DATA_SPECIFIED &&
		    (oaep->pSourceData != NULL || oaep->ulSourceDataLen != 0)) {
			os_log_debug(logsys, "Error: invalid OAEP source "
				     "data specified (%p, %d)",
				     oaep->pSourceData,
				     (int) oaep->ulSourceDataLen);
			return false;
		}

		/*
		 * Find the appropriate mechanism to return that matches
		 * the MGF and the hash algorithm.
		 */

		for (i = 0; i < keychain_param_map_size; i++) {
			if (mptr->mechanism ==
					keychain_param_map[i].base_type &&
			    oaep->hashAlg == keychain_param_map[i].hash_alg &&
			    oaep->mgf == keychain_param_map[i].mgf)
				goto found;
		}

		os_log_debug(logsys, "Error: No valid mapping for OAEP "
			     "parameters: hashAlg = %#x, mgf = %#x",
			     (unsigned int) oaep->hashAlg,
			     (unsigned int) oaep->mgf);

		return false;

	case PSS:
		/*
		 * Slightly easier; make sure the parameter structure
		 * is the right size, then just check for matching
		 * parameters
		 */

		if (mptr->pParameter == NULL || 
		    mptr->ulParameterLen != sizeof(*pss)) {
			os_log_debug(logsys, "Error: PSS parameter invalid "
				     "(pParameter = %p, len = %d)",
				     mptr->pParameter,
				     (int) mptr->ulParameterLen);
			return false;
		}

		pss = (CK_RSA_PKCS_PSS_PARAMS_PTR) mptr->pParameter;

		for (i = 0; i < keychain_param_map_size; i++) {
			if (mptr->mechanism ==
					keychain_param_map[i].base_type &&
			    pss->hashAlg == keychain_param_map[i].hash_alg &&
			    pss->mgf == keychain_param_map[i].mgf &&
			    pss->sLen == keychain_param_map[i].slen)
				goto found;
		}

		os_log_debug(logsys, "Error: No valid mapping for PSS "
			     "parameters: hashAlg = %#x, mgf = %#x, slen = %d",
			     (unsigned int) pss->hashAlg,
			     (unsigned int) pss->mgf, (int) pss->sLen);

		return false;
	}

found:
	if (encalg) {
		*encalg = keychain_param_map[i].encalg ?
					*keychain_param_map[i].encalg : NULL;
		os_log_debug(logsys, "Encryption algorithm chosen: %{public}@",
			     *encalg);
	}

	if (signalg) {
		*signalg = keychain_param_map[i].signalg ?
					*keychain_param_map[i].signalg : NULL;
		os_log_debug(logsys, "Signing algorithm chosen: %{public}@",
			     *signalg);
	}

	if (dsignalg) {
		*dsignalg = keychain_param_map[i].dsignalg ?
					*keychain_param_map[i].dsignalg : NULL;
		os_log_debug(logsys, "Digest signing algorithm chosen: "
			     "%{public}@", *dsignalg);
	}

	if (digest) {
		*digest = keychain_param_map[i].hash_alg;
		os_log_debug(logsys, "Digest algorithm chosen: %{public}s",
			     getCKMName(*digest));
	}

	return true;
}
	

/*
 * Add to a dictionary (and create the dictionary if needed)
 */

static bool
add_dict(CFMutableDictionaryRef *md, const void *key, const void *value)
{
	if (*md == NULL) {
		*md = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
						&kCFTypeDictionaryKeyCallBacks,
						&kCFTypeDictionaryValueCallBacks);
		if (*md == NULL) {
			os_log_debug(logsys, "query dictionary creation "
				     "returned NULL");
			return false;
		}
	}

	CFDictionaryAddValue(*md, key, value);

	return true;
}

/*
 * Free an array of characters (usually something returned by a prefkey
 * function)
 */

static void
array_free(char **array)
{
	char **p = array;

	if (! array)
		return;

	while (*p != NULL)
		free(*p++);

	free(array);
}

/*
 * Free a session
 */

static void
sess_free(struct session *se)
{
	int i;

	LOCK_MUTEX(se->mutex);

	for (i = 0; i < se->search_attrs_count; i++)
		free(se->search_attrs[i].pValue);

	free(se->search_attrs);

	if (se->key)
		CFRelease(se->key);

	if (se->mdc) {
		/*
		 * At this point we need to finalize the digest, but
		 * just discard the results.
		 */

		unsigned char *digest;
		unsigned int size;

		cc_md_final(se->mdc, &digest, &size);
		free(digest);
	}

	if (se->token)
		slot_entry_free(se->token, true);

	UNLOCK_MUTEX(se->mutex);
	DESTROY_MUTEX(se->mutex);
	free(se);
}

/*
 * Logout from our token
 */

static void
token_logout(struct slot_entry *token)
{
	/*
	 * Log out from all identities on a single token; since we
	 * now share a lacontext across identities, we only need to
	 * do this once.
	 */

	if (token->lacontext) {
		lacontext_logout(token->lacontext);
		token->lacontext = NULL;
	}

	token->logged_in = false;
}
