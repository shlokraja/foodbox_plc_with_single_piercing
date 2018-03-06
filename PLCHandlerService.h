#include <stdio.h>
#include <time.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <jansson.h>
#include <signal.h>

#include "plc.h"
#include "PLCVariables.h"

// Windows <-> Linux equivalence
#ifndef BOOL
	#define BOOL bool
#endif
#ifndef TRUE
	#define TRUE 1
#endif
#ifndef FALSE
	#define FALSE 0
#endif

// Item dispense stages
// Stages:
// 1 - Picked from Slot; 2 - Staging Area; 3 - Rotary, 4 - Piercing
// .. 5 - Microwave Oven Front; 6 - In Microwave Oven; 7 - MicroWave Heating
// .. 8 - Lane Change [optional]; 9 - Lane End [delivery point]
enum
{
	PENDING = -1,
	STARTED,
	STAGE1, /* PICKED */
	STAGE2, /* STAGING */
	STAGE3, /* ROTARY */
	STAGE4, /* PIERCING */
	STAGE5, /* MIC FRONT */
	STAGE6, /* MIC IN */
	STAGE7, /* MIC HEATING: Must be == 7 */
	STAGE8, /* LANE CHANGE [Optional] */
	STAGE9, /* LANE END == DELIVERY POINT */
	TIMEOUT /* DISPENSE TIMED OUT! */
};

// Stage 9 is Completion
#define COMPLETE STAGE9

// 9 stages are the actual machine reported stages/states
#define MACHINESTAGECOUNT 9

// 5000 Max items per dispenser
#define MAXITEMS 5000

// 1025 MAX LENGTH OF VARIABLE Name (1024 + 1 NULL char)
#define MAXPLCVARNAMELEN 1025

// One-second timeout for PLC reads
#define PLCTIMEOUT 1000

// Main loop polling (PLC/OrderQueue) interval in seconds
#define MAINDELAYSECONDS 5

// Pending item timeout (waiting for it to dispense) in seconds - 25 mins
// ...this is during machine fault situations only, when the pending items
// ...can atmost wait until the staff fixes the issue, a max of 25 mins
#define ITEMREADINESSTIMEOUT 1500

// Max bytes readable by PLC
#define MAXPLCREAD 8192

// Log Priority is 5 = super deep
#define LOGPRIORITY 4

// String Names of stages - 11 of them
// char szStages[][]= {"PENDING", "STARTED", "PICKED", "STAGING AREA",
// "ROTARY", "PIERCING", "MICROWAVE FRONT", "IN MICROWAVE", "MICROWAVE HEATING",
// "LANE CHANGE", "DELIVERY PT"};

// Structure used to populate item-status-list
// ...for pending/in-progress dispense items
typedef struct
{
	// Auto-ID: Unique per-dispense-id, 10 numeric digits
	char szDispenseID[11];

	// Order Stub - 49 chars
	char szOrderStub[60];

	// Stage of Dispense
	int iDispenseStage;

	// Dispense variant [only for microwave heating tracking currently]
	int iVariant;

	// Timer string [all timer values for timed stages of dispense, separated by | ]
	char szTimerString[1024];

	// Dispense-start time, used for timeout-computation
	time_t ttStartTime;
} ItemStatusNode, *pItemStatusNode;


// Linked List Node
typedef struct NodeStruct
{
	// Pointer to Payload
	ItemStatusNode PayLoad;

	// Pointer to next node
	struct NodeStruct *pNext;
} Node, *pNode;

// Item-Dispense Struct
// This stores data to be used to ask the PLC to dispense an item
typedef struct
{
	// DispenseID
	char szDispenseID[11];

	// Order Stub - 49 chars
	char szOrderStub[60];
}ItemDispenseData, *pItemDispenseData;

// Compartment Info struct
// Stores config info, variables, etc. for a single compartment
// of the dispensers.
// We now have only 1 compartment - for all dispensers
typedef struct
{
	// # of total slots in this compartment
	int iSlotCount;

	// Dispense readiness state PLC variable
	char szDispenseReadinessVar[MAXPLCVARNAMELEN];

	// Name of variable to post order to PLC
	char szOrderVar[MAXPLCVARNAMELEN];

	// Name of variable to check Door Closure State (for scan)
	char szDoorClosedVar[MAXPLCVARNAMELEN];

	// Name of variable to check OK to Open Door State (for scan)
	char szOKToOpenDoorVar[MAXPLCVARNAMELEN];

	// Scan started variable name
	char szScanStartVar[MAXPLCVARNAMELEN];

	// Sync Mode :: Scan complete var
	char szSyncScanCompleteVar[MAXPLCVARNAMELEN];

	// Sync Mode :: Name of variable for scanning 1 barcode + slotnumber
	char szSyncBarCodeSlotNumberVar[MAXPLCVARNAMELEN];

	// Async Mode :: Scan complete var
	char szAsyncScanCompleteVar[MAXPLCVARNAMELEN];

	// Async Mode :: Name of variable for barcode array [async mode]
	char szAsyncBarCodeArrayVar[MAXPLCVARNAMELEN];
}CompartmentInfo, *pCompartmentInfo;

// Configuration Info Struct
// This will store the global config info for the outlet
// ..in which this instance of PLCHandlerService is running
typedef struct
{
	char szPLCIP[16];						// IP address of PLC server
	int iPLCPort;								// Port of PLC server
	int iLaneCount;             // Number of delivery lanes (output) - upto 4, usually 2
	BOOL bAsyncScan;            // If the machine has async scanning enabled
	int iSlotCount;							// Slot count
	int iDispenseTimeout;				// Timeout (in seconds) for item once it has started dispensing
	int iPLCType; 							// PLC Type 0: ControlLogix, 1: MicroLogix etc.
} ConfigInfo, *pConfigInfo;

// Struct for curl reads
struct MemoryStruct {
	char *pcBuffer;
	size_t stSize;
};

// Struct for posting item status data to local cloud
typedef struct
{
	char szOrderStub[1024];
	char szDispenseID[1024];
	int iStatus;
	char szTimerString[1024];
} ItemStatusData, *pItemStatusData;
