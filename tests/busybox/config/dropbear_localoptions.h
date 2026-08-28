#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SVR_ROOT_LOGIN 1
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_SFTPSERVER 0
#define DROPBEAR_REEXEC 0

/* Keep the cycle-accurate test server small. The image and authorized key are
 * both Ed25519, the client uses X25519 key exchange, and `none` is selected as
 * the transport cipher. AES-128 remains compiled as Dropbear's required
 * encrypted fallback. */
#define DROPBEAR_AES256 0
#define DROPBEAR_CHACHA20POLY1305 0
#define DROPBEAR_RSA 0
#define DROPBEAR_ECDSA 0
#define DROPBEAR_SK_KEYS 0
#define DROPBEAR_DH_GROUP14_SHA256 0
#define DROPBEAR_SNTRUP761 0
#define DROPBEAR_MLKEM768 0
#define DROPBEAR_ECDH 0
#define DROPBEAR_CURVE25519 1
#define DROPBEAR_ED25519 1
#define DROPBEAR_DELAY_HOSTKEY 0

/* Test-only cleartext SSH transport for the cycle-accurate simulator. SSH host
 * and user-authentication state machines remain enabled, but this profile has
 * no cryptographic authentication, confidentiality, or packet MAC; it is
 * deliberately unsuitable outside an isolated test link. */
#define DROPBEAR_NONE_CIPHER 1

/* Static glibc cannot resolve /etc/passwd through NSS in the freestanding
 * MikOS image.  This fixed test appliance has exactly one login account, so
 * provide that account without weakening username checks in normal builds. */
#define DROPBEAR_MIKOS_TEST_ACCOUNT 1

/* A cycle-level RV32 Ed25519 operation takes close to an hour on the current
 * native model.  For the explicitly insecure `none` transport, replace it
 * with a transcript-and-public-key-bound proof shared by the paired test
 * client.  Encrypted transports retain upstream Ed25519. */
#define DROPBEAR_MIKOS_NONE_ED25519_PROOF 1

/* The upstream five-minute pre-authentication limit is too short for the
 * remaining guest work in a cycle-accurate RV32 simulator. */
#define AUTH_TIMEOUT 7200

/* MikOS serializes the listener and its children, but retains enough nested
 * address-space snapshots for listener -> SSH session -> remote command.
 * Keep Dropbear's normal connection fork so the listener survives and can
 * accept subsequent sessions. */
#define DEBUG_NOFORK 0
