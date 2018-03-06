/* plc.h - Header file for the PLCIO UNIX Communications Library */

/* PLCIO 4.0, Written by Byron Stanoszek  - March 4, 2005
   Based on prior code written by William Hays & John McCarthy */

/*************************************************************************
 *          COPYRIGHT (C) 1992-2014 COMMERCIAL TIMESHARING INC.          *
 *                                                                       *
 *                         ALL RIGHTS RESERVED                           *
 *                                                                       *
 * THIS PROGRAM IS AN UNPUBLISHED WORK FULLY PROTECTED BY THE COPYRIGHT  *
 * LAWS OF THE UNITED STATES AND ELSEWHERE AND IS CONFIDENTIAL AND       *
 * PROPRIETARY MATERIAL BELONGING TO THE COPYRIGHT OWNER. REPRODUCTION,  *
 * INCLUDING TRANSLATION TO ANOTHER PROGRAM LANGUAGE, IS STRICTLY        *
 * PROHIBITED WITHOUT PRIOR WRITTEN CONSENT.                             *
 *************************************************************************/

#ifndef __PLC_H_
#define __PLC_H_

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(__GNUC__)
# if defined(__WIN32__) || defined(WIN32) || defined(_WIN32)
#  define extern extern __declspec(dllimport)
# endif
#endif

#if defined(__WIN32__) || defined(WIN32) || defined(_WIN32)
# include <winsock.h>
#else
# include <sys/socket.h>
#endif

/* Global Variables */
extern int j_plcio_open_timeout;  // Overrides timeout for plc_open() (in ms)
extern int j_plcio_ipaddr;        // Local IP Address for unsolicited reads
extern int j_plcio_logsize;       // Maximum log file size before rotation
extern char *plcio_version;       // Runtime library version and date of PLCIO

/* Maximum message size for any PLCIO function */
#define PLC_CHAR_MAX 8192

/* Timeouts are always specified in milliseconds */
#undef HZ
#define HZ 1000

/* plc_conv() types */
#define PLC_TOCPU  0  // Used when converting a msg from PLC to the application
#define PLC_TOPLC  1  // Used when sending a data buffer from an app to the PLC

/* plc_conv() special format constants */
#define PLC_CVT_WORD ((char *) 0)  // Buffer contains all 16-bit ints (default)
#define PLC_CVT_NONE ((char *)-1)  // Perform no conversion

/* plc_log_init() special constants */
#define PLC_LOG_NONE ((char *) 0)  // No logging requested
#define PLC_LOG_TTY  ((char *)-1)  // Output all logging to the console

/* plc_status() return codes (currently unused) */
#define PLC_STOP   0  // PLC is Stopped
#define PLC_RUN    1  // PLC is in Run Mode

/* PLC Message Styles */
#define PLC_ID_PLC2      1  // Allen-Bradley PLC-2
#define PLC_ID_PLC5      2  // Allen-Bradley PLC-5
#define PLC_ID_SLC500    3  // Allen-Bradley SLC 500
#define PLC_ID_S7_200    4  // Siemens S7-200
#define PLC_ID_S7_300    5  // Siemens S7-300
#define PLC_ID_S7_400    6  // Siemens S7-400
#define PLC_ID_OMRON_CS  7  // Omron CJ/CS mode
#define PLC_ID_OMRON_CV  8  // Omron CV mode
#define PLC_ID_PLC5_WR   9  // Allen-Bradley PLC-5 Word Range

/* plc_read()/plc_write() operations for `j_op' */
#define PLC_RCOIL        1   // Read coil registers (2 bytes)
#define PLC_RBYTE        2   // Read string registers (up to 82 bytes)
#define PLC_RREG         3   // Read word registers (2 bytes)
#define PLC_RLONG        4   // Read floating-point registers (4 bytes)
#define PLC_RCHAR        9   // Read ascii registers (1 byte)
#define PLC_RLONGINT     11  // Read double-word registers (4 bytes)

#define PLC_WCOIL        5   // Write coil registers (2 bytes)
#define PLC_WBYTE        6   // Write string registers (up to 82 bytes)
#define PLC_WREG         7   // Write word registers (2 bytes)
#define PLC_WLONG        8   // Write floating-point registers (4 bytes)
#define PLC_WCHAR        10  // Write ascii registers (1 byte)
#define PLC_WLONGINT     12  // Write double-word registers (4 bytes)


/* Unsolicited Packet via plc_receive() */
typedef struct s_plcslave {
  int j_length;		// Message length
  int j_type;		// PLC_SLAVE_* from below; type of message received
  int j_offset;		// Offset or element number
  int j_ipaddr;		// IP Address of Remote Host (Network byte order)
  int j_fileno;		// Allen-Bradley file number
  int j_sequence;	// Streaming I/O sequence number

  int aj_reserved[2];	// Future expansion
} PLCSLAVE;

/* Bitmask for message types received via plc_receive() (j_type above) */
#define PLC_SLAVE_WREGS		0x0001  // Unsolicited write multiple registers
#define PLC_SLAVE_RREGS		0x0002  // Unsolicited read multiple registers
#define PLC_STREAM_INPUT	0x0004  // Input from Streaming I/O

/* j_op response codes for plc_reply() */
#define PLC_SLAVE_ACK		1       // Send Slave ACK
#define PLC_SLAVE_NAK		0       // Send Slave NAK


/* Soft Point Table Record */
struct s_pointrec {
  char *pc_point;	// Point name
  char *pc_addr;	// Actual PLC address
  int j_bytes;		// Length of address
  int j_type;		// Bitmask indicating Readable and/or Writable
};

/* Members of j_type above */
#define PLC_CFGT_READ   0x1  // Soft Point is readable
#define PLC_CFGT_WRITE  0x2  // Soft Point is writable


typedef struct s_plc PLC;

/* PLCIO_FUNCS: Holds the references to the dynamically-loaded module */
typedef struct s_plcio_funcs {
  struct s_plcio_funcs *next;
  void *handle;
  char **ppc_mod_version;
  char ac_ident[24];

  int (*pf_open)(PLC *plc_ptr, char *pc_ident);
  int (*pf_close)(PLC *plc_ptr);
  int (*pf_validaddr)(PLC *plc_ptr, char *pc_addr, int *pj_size,
                      int *pj_domain, int *pj_offset);
  int (*pf_readwrite)(int j_read, PLC *plc_ptr, int j_op, char *pc_addr,
                      void *p_buf, int j_bytes);
  int (*pf_receive)(PLC *plc_ptr, int j_accept, PLCSLAVE *ps_slave,
                    void *p_buf, int j_bytes);
  int (*pf_reply)(PLC *plc_ptr, int j_op, void *p_buf, int j_bytes);
  int (*pf_fd_set)(PLC *plc_ptr, fd_set *ps_readset, int *pj_nfds);
} PLCIO_FUNCS;

/* PLC Structure */
struct s_plc {
  int j_softplc;		// 1=Lookup addrs in point configuration table
  int j_mode;			// 0=App is master, 1=App is Slave, 2=Stream
  int j_plctype;		// PLC's endianness: '9'=Big-endian, 'I'=Little
  int j_cputype;		// Application/CPU's endianness
  int j_verbose;		// Module verbosity level
  int j_open_timeout;		// Timeout on plc_open() in seconds
  int aj_reserved[15];		// Future expansion

  /* Module-specific Data */
  PLCIO_FUNCS *pfuncs;		// Pointer to module _plc_*() functions
  void *pvoid;			// Module-specific communications structure

  /* Soft Point Config */
  struct s_pointrec *ps_points;	// List of available soft points
  int j_points;			// Number of elements in above array

  /* Error Reporting */
  int j_errno;			// UNIX Error Number
  int aj_errorval[8];		// Additional error-specific parameters
  char ac_errmsg[80];		// Error string
  int j_error;			// PLC Error Number
};

/* Members of j_mode above */
#define PLC_MASTER	0
#define PLC_SLAVE	1
#define PLC_STREAM	2

/* Required for error reporting after plc_open() fails */
extern PLC *plc_open_ptr;


/**** Exported Library Functions *********************************************/

/* PLC Functions */
extern PLC *plc_open(char *);
extern int plc_close(PLC *plc_ptr);
extern int plc_validaddr(PLC *plc_ptr, char *pc_addr, int *pj_size,
                         int *pj_domain, int *pj_offset);
extern int plc_read(PLC *plc_ptr, int j_op, char *pc_addr, void *p_buf,
                    int j_bytes, int j_timeout, char *pc_format);
extern int plc_write(PLC *plc_ptr, int j_op, char *pc_addr, void *p_buf,
                     int j_bytes, int j_timeout, char *pc_format);
extern int plc_receive(PLC *plc_ptr, int j_op, PLCSLAVE *ps_data, void *p_buf,
                       int j_bytes, int j_timeout);
extern int plc_reply(PLC *plc_ptr, int j_op, void *p_buf, int j_bytes,
                     int j_timeout);
extern int plc_conv(PLC *plc_ptr, int j_type, void *p_buf, int j_len,
                    char *pc_format);
extern int plc_fd_set(PLC *plc_ptr, fd_set *ps_readset, int *pj_max_fd);
extern int plc_fd_isset(PLC *plc_ptr, fd_set *ps_readset);

/* Configuration */
extern void plc_set_cfgfname(const char *pc_path, const char *pc_file);

/* Error Reporting */
extern void plc_set_error(PLC *plc_ptr, int j_error, const char *format, ...);
extern void plc_clear_errors(PLC *plc_ptr);
extern void plc_print_error(PLC *plc_ptr, const char *pc_string);
extern int  plc_error(PLC *plc_ptr, int j_level, char *pc_buf, int j_len);

/* Logging */
extern void plc_log_init(const char *pc_buf);
extern int  plc_log(const char *fmt, ...);

#if !defined(__GNUC__)
# if defined(__WIN32__) || defined(WIN32) || defined(_WIN32)
#  undef extern
# endif
#endif

#ifdef __cplusplus
}
#endif


/**** PLCIO Error Codes ******************************************************/

#define PLCE_OK			0  // No error

/* Library errors */
#define PLCE_OPEN_CONFIG	1  // Error while opening Soft PLC config file
#define PLCE_INVAL_SOFTPLC	2  // No such PLC found in Soft PLC config file
#define PLCE_WRONG_TYPE		3  // Soft PLC def. has wrong master/slave type
#define PLCE_INVAL_MODULE	4  // Could not find or load PLC module
#define PLCE_NO_MEMORY		5  // Memory allocation error
#define PLCE_MISSING_FUNCS	6  // Open/Close missing in library module
#define PLCE_OPEN_POINTCFG	7  // Error opening PLC Point Config file
#define PLCE_MOD_VERSION	8  // Library and module version mismatch

/* Function errors */
#define PLCE_NULL		10 // PLCIO Function called with NULL plc_ptr
#define PLCE_NO_SUPPORT		11 // PLCIO Function unsupported by this module
#define PLCE_DUPLICATE_CLOSE	12 // Duplicate plc_close() detected
#define PLCE_INVALID_POINT	13 // Read/Write supplied with invalid address
#define PLCE_INVALID_LENGTH	14 // Read/Write < 1 or > PLC_CHAR_MAX bytes
#define PLCE_BAD_SOFTPOINT	15 // Point does not exist in Soft PLC config
#define PLCE_NO_READ		16 // Point not configured for Reading
#define PLCE_NO_WRITE		17 // Point not configured for Writing
#define PLCE_CONV_FORMAT	18 // Error in the format conversion string
#define PLCE_PARSE_ADDRESS	19 // Parse error while reading PLC address
#define PLCE_BAD_ADDRESS	20 // No such address/point defined by PLC
#define PLCE_REQ_TOO_LARGE	21 // Length is too big for target point
#define PLCE_BAD_REQUEST	22 // Message length not divisible by elemsize
#define PLCE_ACCESS_DENIED      23 // Access denied to read/write PLC address
#define PLCE_INVALID_MODE	24 // Wrong master/slave/stream mode
#define PLCE_RECV_TOO_LARGE	25 // Received unsol message > requested bytes
#define PLCE_INVALID_OP		26 // j_op==PLC_RREGS on a plc_write(), etc.
#define PLCE_INVALID_REPLY	27 // Did plc_reply() without a plc_receive()
#define PLCE_REPLY_TOO_LARGE	28 // Can't fit response in single reply packet
#define PLCE_INVALID_DATA	29 // Write supplied with bad data for point

/* Transport errors */
#define PLCE_PARSE_IDENT	40 // Parse error while reading ident string
#define PLCE_MISSING_HOST	41 // No hostname specified in plc_open() call
#define PLCE_UNKNOWN_HOST	42 // Could not resolve remote host name
#define PLCE_BAD_TCP_PORT	43 // TCP Port specified not in 1-65535 range
#define PLCE_OPEN_SOCKET	44 // Could not open network socket
#define PLCE_CONNECT		45 // Could not connect() to remote PLC
#define PLCE_COMM_SEND		46 // Transport error while sending data to PLC
#define PLCE_COMM_RECV		47 // Transport error while waiting for resp.
#define PLCE_TIMEOUT		48 // Reached timeout before data was sent/recv
#define PLCE_SERIAL_PARAM	49 // Invalid serial device parameter
#define PLCE_OPEN_SERIAL	50 // Could not open serial device
#define PLCE_BIND		51 // Could not bind socket to local port
#define PLCE_SELECT		52 // Transport error during select() call
#define PLCE_MSG_TRUNC		53 // Received truncated comm. packet from PLC
#define PLCE_MULTICAST		54 // Could not join a multicast group
#define PLCE_NO_ENDPOINT	55 // No endpoint established for Streaming I/O 

/* Remote PLCIOD concentrator errors */
#define PLCE_REMOTE_PROTO	100 // Protocol error while talking to plciod

/**** Module-specific errors ****/

/* CIP Transport */
#define PLCE_CIP_COMM_ERROR	200 // Communications/routing error
#define PLCE_CIP_CPU_SLOT	201 // Failed to autodetect slot# of CPU
#define PLCE_CIP_BAD_TAG	202 // Bad packet received while reading tags
#define PLCE_CIP_IO_ADDR	203 // Remote host did not specify I/O address
#define PLCE_CIP_RPI_EXCEEDED	204 // Time since last request exceeds RPI

/* ENIP Unsolicited */
#define PLCE_ENIP_INIT_ERROR	201 // Invalid enipd protocol or Channel ID

/* DF1 Protocol - PCCC errors (shared with many modules) */
#define PLCE_DF1_PCCC_ERROR	210 // PCCC Read/Write error from PLC
#define PLCE_DF1_BAD_ADDR	211 // Received PCCC command for a bad address
#define PLCE_DF1_BAD_MSG	212 // Received PCCC command with bad params

/* FINS Protocol */
#define PLCE_FINS_PLC_ERROR	210 // FINS error from PLC
#define PLCE_FINS_BANK		211 // Extended memory bank not available
#define PLCE_FINS_SYMBOL_TABLE	212 // Error decompressing PLC Symbol Table
#define PLCE_FINS_TCP_NODE	213 // No TCP nodes available on router

/* Modbus+ Ethernet */
#define PLCE_MOD_PLC_ERROR	202 // Read/Write error from PLC

/* Siemens Step5 (AS511 & S5-AP) */
#define PLCE_S5_PLC_ERROR	200 // Request not accepted by PLC
#define PLCE_S5_UNDEF_BLOCK	202 // Undefined Data Block number on PLC

/* Siemens Step7 */
#define PLCE_S7_TSAP_REFUSED	200 // Connection via ISO 8073 refused
#define PLCE_S7_OPEN_ERROR	201 // Unexpected data during Open Session req.
#define PLCE_S7_PLC_ERROR	202 // Read/Write error from PLC
#define PLCE_S7_UNDEF_BLOCK	203 // Undefined Data Block number on PLC
#define PLCE_S7_CPU_SLOT	204 // Bad CPU slot# or failed to autodetect

/* Virtual PLC */
#define PLCE_VIRTUAL_TMPFILE	200 // Failed to open /tmp/raw1 or /tmp/raw2

#endif  /* __PLC_H_ */
