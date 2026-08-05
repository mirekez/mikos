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

/* Curve25519 and Ed25519 execute in a cycle-accurate RV32 simulator.  The
 * upstream five-minute pre-authentication limit can expire while a valid key
 * exchange is still computing, so give the interactive regression enough
 * simulated time to complete it. */
#define AUTH_TIMEOUT 7200

/* MikOS currently serializes a background child against its shell parent.
 * Handle one accepted SSH connection in the listener process so transport
 * and public-key authentication can be exercised before nested fork lands. */
#define DEBUG_NOFORK 1
