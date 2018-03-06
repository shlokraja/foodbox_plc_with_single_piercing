
// Just one include file - everything is referenced there
#include "PLCHandlerService.h"

/// Global functions
// Descriptions are at function implementations [purpose, params, return value]
void CheckItemsForTimeouts();
void DispenseItemFromList(pItemDispenseData pItem);
void InitializeCompartmentInfo();
void WaitTillPLCReady(PLC *pPLC);
void ProcessMachineStateData();
void *ScanWorkerFunction(void *pArg);
BOOL GetScanStatus(PLC *pPLC);
void GetConfigFromLocalCloud(ConfigInfo *cfgInfo);
void DoLog(const char *pszLogMsg, int iPriority = 0);
pNode InsertListNode(char *pszDispenseID, int iStatus, char *pszOrderStub);
char *substr(char *pszString, int iStartIdx, int iNumChars);
void PurgeItemFromStatusList(char *pszDispenseID);
void PostTotalStockToLocalCloud();
void UpdateDispenserStock(char pszSlotArray[][10], char pszBarCodeArray[][35], int iNumScanned);
pItemDispenseData *GetNewItemsFromLocalCloud();
void PostItemStatusToLocalCloud(char *pszOrderStub, char *pszDispenseID, int iStatus, char *pszTimerString = NULL);
void *SendScanStartSignalToLocalCloud(void *pArg);
void ProcessCfgResponse(ConfigInfo *pCfgInfo, struct MemoryStruct *pData);
static size_t CurlWriterCallback(void *pContents, size_t stSize, size_t stNum, void *pUser);
void PopulateStageVarsAndTypes();
void WriteCompletionStatusToFile(char *pszOrderStub, int iLane);
void *PostItemStatusWorker(void *pData);

// externs
extern void DisconnectFromPLC(PLC *pPLC);
extern PLC *ConnectToPLC(char *pszIP, int iPort, BOOL bMicroLogix);
extern char *ReadVarFromPLC(PLC *pPLC, char *pszVarName, char cVarType);
extern void *WriteVarToPLC(PLC *pPLC, char *pszVarName, char *pszVal, int iLen);

/// START Global Variables ////////////////////////////////////////
// Linked List head/tail
pNode g_pHead = NULL;

// Log files
FILE *g_pLogFile = NULL;

// Config info
ConfigInfo g_CfgInfo = {0};

// Mutexes (LOCKs) for Log File + PLC + stock table
// Log file write operations
// ..and PLCIO library calls
// ..as both may be performed from multiple threads [and PLCIO lib isnt thread-safe]
// ..stock table as scans into stock table and reads from stock table can happen
// ..from multiple threads
pthread_mutex_t g_logLock, g_plcLock, g_stockLock;

// Scan worker thread ID, scan signal thread id
pthread_t scanThreadID;
pthread_t scanSignalThreadID;

// AppDone flag - never signalled, but good to put in
// ..all threads with eternal loops quit when this flag is set to TRUE
// ..better than doing WHILE(TRUE) loops which future devs may curse us for
BOOL g_bAppDone = FALSE;

// PLC Pointers - one for orders, one for scan
PLC *g_pOrderPLC = NULL, *g_pScanPLC = NULL;

// Compartment Info [upto 4]
CompartmentInfo g_CompInfo = {0};

// Stock Table
int g_iBarCodeCount = {0};
char g_szBarCodeArray[MAXITEMS][35] = {0}; // MAXITEMS barcodes, 25 chars including NUL
char g_szSlotStringArray[MAXITEMS][2000] = {0}; // MAXITEMS barcodes, 2000 char slot string
int g_iSlotCountArray[MAXITEMS] = {0};

// Stage Variables array
// ..this is a 3-D array {Stage, Variants for Stage, Variable-Name-Char-Array}
// ..since each stage may be seen at different dispensers, microwaves, etc [variants]
char g_szStageVars[10][4][200] = {0};
char g_szStageTypes[10][4][1] = {0};

// Local Cloud IP:Port
char g_szIPPort[22] = {0};

// Wipe off status
BOOL g_bWipeOffDone = FALSE;

// Log counter + priority
int g_iLogLineCounter = 0;
int g_iLogPriority = LOGPRIORITY;

int g_iStatusListNodeCount = 0;

// Array that tracks dispense-id dispense start
BOOL g_bDispenseIDStarted[50000] = {0};

/// END Global Variables //////////////////////////////////////////




// Main function of PLC Handler Service
int main()
{
	// Initialize mutexes
	pthread_mutex_init(&g_logLock, NULL);
	pthread_mutex_init(&g_plcLock, NULL);
	pthread_mutex_init(&g_stockLock, NULL);

	// Avoid SIGPIPE CRASHES
	signal(SIGPIPE, SIG_IGN);

	DoLog("Main:: PLCHandlerService starting");

	// Initialize curl
	curl_global_init(CURL_GLOBAL_ALL);

	// Get Config from LocalCloud
	// ...this function will keep retrying until it gets the configuration
	GetConfigFromLocalCloud(&g_CfgInfo);

	DoLog("Main:: Got Configuration Info...");

	char szLogMsg[4096];
	sprintf(szLogMsg, "Main:: PLCIP [%s] Lanes: %d Async: %s Slots: %d",
			g_CfgInfo.szPLCIP, g_CfgInfo.iLaneCount,
			g_CfgInfo.bAsyncScan?"yes":"no", g_CfgInfo.iSlotCount);	
	DoLog(szLogMsg, 1);

	// Populate array of stage-var-strings [indexed from 1 onwards]
	// ...this is dependent on CfgInfo as the var names vary between
	// ...MicroLogix/ControlLogix
	PopulateStageVarsAndTypes();

	// Spawn Scan Worker Thread
	pthread_create(&scanThreadID, NULL, &ScanWorkerFunction, NULL);

	/// Main Thread is for Order Processing [No Scan Handling]
	// Connect to ControlLogix/MicroLogix PLC
	// ... [the function will retry until connection succeeds]
	if (g_CfgInfo.iPLCType == 0)
		g_pOrderPLC = ConnectToPLC(g_CfgInfo.szPLCIP, g_CfgInfo.iPLCPort, FALSE); // ControlLogix
	else
		g_pOrderPLC = ConnectToPLC(g_CfgInfo.szPLCIP, g_CfgInfo.iPLCPort, TRUE); // MicroLogix

	DoLog("Main:: OrderPLC Connected, Waiting for POWER ON + READY");

	// Wait till PLC ready (Power On + Always On must be set)
	WaitTillPLCReady(g_pOrderPLC);

	DoLog("Main:: OrderPLC POWER ON + READY");

	/// Compartment Preparation
	// Just gets the variables and puts them in our
	// dispenser struct for easy access
	InitializeCompartmentInfo();

	int iItemIdx;
	pItemDispenseData *pNewItemList = NULL, pListItem = NULL;

	// Dispense Loop
	while (!g_bAppDone)
	{
		// Do we have no new item list? Or do we have no remaining-items?
	 	if (!pNewItemList || !pListItem)
		{
			DoLog("Dispense Loop:: Getting new items from LocalCloud", 5);

			// Cleanup if required the existing new-item-list
			if (pNewItemList)
			{
				// Cleanup the pointers in array
				for (int i = 0; pNewItemList[i] != NULL; i++)
					delete pNewItemList[i];

				// Cleanup the array itself
				delete []pNewItemList;
			}

			// Fetch dispense-list from local cloud & pickup all items (by DispenseID ASC sorted)
			// ...this function only processes the items whose status is pending
			pNewItemList = GetNewItemsFromLocalCloud();

			// Initialize list item
			pListItem = pNewItemList ? pNewItemList[0] : NULL;

			// Rewind iter-index to first item in list
			iItemIdx = 0;
		}


		char *pszReadyVal = NULL;

		// Do we have an item to dispense?
		if (pListItem)
		{
			DoLog("Dispense Loop:: There is a new item to dispense", 4);

			int iReadinessLoops = 0;

readinesscheck:
			DoLog("Dispense Loop:: Checking dispenser for readiness", 5);

			// Read the dispenser ready-var
			pszReadyVal = ReadVarFromPLC(g_pOrderPLC, g_CompInfo.szDispenseReadinessVar, 'b');

			// We need a valid return value AND it must be == 1 (true)
			if (pszReadyVal && !strcmp(pszReadyVal, "1"))
			{
					DoLog("Dispenser ready; sending item", 1);

					// Send item from list
					DispenseItemFromList(pListItem);

					// Move current-item fwd
					iItemIdx++;

					// Get next 'current-item'
					pListItem = pNewItemList[iItemIdx];

					// Cleanup memory
					delete []pszReadyVal;
			} // end of ready-val presence check
			// We got a non-null result but it was not 1 i.e ready?
			else if (pszReadyVal)
			{
					// Sleep a short while (1 second) before retrying
					sleep(1);

					// Have we been waiting for readiness too long?
					if (++iReadinessLoops > ITEMREADINESSTIMEOUT)
					{
						// Cleanup memory
						delete []pszReadyVal;

						// Expire this item
						char szMsg[1024] = {0};
						sprintf(szMsg, "{Main Loop} Item readiness timeout DispenseID [%s] OrderStub [%s]", pListItem->szDispenseID, pListItem->szOrderStub);
						DoLog(szMsg, 2);

						// Post timeout to LC
						PostItemStatusToLocalCloud(pListItem->szOrderStub, pListItem->szDispenseID, TIMEOUT);

						// Move current-item fwd
						iItemIdx++;

						// Get next 'current-item'
						pListItem = pNewItemList[iItemIdx];

						// End of list?
						if (!pListItem)
							// Loop forward the 'while' loop
							continue;

						// loop forward to retry the remaining items (so they can get timedout also)
						goto readinesscheck;
					} // end readiness wait timeout check
					else
					{
						// Cleanup memory
						delete []pszReadyVal;

						DoLog("Dispense Loop:: [item waiting to dispense]", 5);

						// Process Machine State Data
						// ...this updates the item-status-list with any updates to item-stage
						// ...based on the machine state data
						ProcessMachineStateData();

						// Check for timeouts
						CheckItemsForTimeouts();

						// loop forward to retry
						goto readinesscheck;
					} // end of else: readiness wait timeout didnt happen
			} // end of else :: non-null result but not 1 i.e ready
		} // end of new-item-list handling loop


		// Check for timeouts
		CheckItemsForTimeouts();

		// Sleep a while - MAINDELAYSECONDS * 0.8 seconds, doing PMSD each 0.8sec
		for (int i = 0; i < MAINDELAYSECONDS; i++)
		{
			// Process Machine State Data
			// ...this updates the item-status-list with any updates to item-stage
			// ...based on the machine state data
			ProcessMachineStateData();

			// Sleep 0.8 seconds
			usleep(0.8 * 1000000);
		} // end sleep + pmsd loop

	} // end of main state-machine read loop


	/// Application is done, cleanup time
	DoLog("Main:: Service done, doing cleanup");

	// Disconnect from PLC
	DisconnectFromPLC(g_pOrderPLC);

	DoLog("Main:: Disconnected from OrderPLC");

	// Cleanup if required the existing new-item-list
	if (pNewItemList)
	{
		// Cleanup the pointers in array
		for (int i = 0; pNewItemList[i] != NULL; i++)
			delete pNewItemList[i];

		// Cleanup the array itself
		delete []pNewItemList;
	}

	// Cleanup curl
	curl_global_cleanup();

	// Clean up - this is never really called
	// ..in the current logic flow
	pthread_mutex_destroy(&g_stockLock);
	pthread_mutex_destroy(&g_logLock);
	pthread_mutex_destroy(&g_plcLock);

	return 0;
} // end of main func

// Fills the stage-vars & stage-types arrays
// These are the dispense stage PLC variable names
// No parameters, no return value
void PopulateStageVarsAndTypes()
{
	// PLC Type 0 == ControlLogix, non 0 == MicroLogix
	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[1][1], d1stringBCONPickedItem);
	else
		strcpy(g_szStageVars[1][1], d1MLstringBCONPickedItem);

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[2][1], d1stringBCONStagingItem);
	else
		strcpy(g_szStageVars[2][1], d1MLstringBCONStagingItem);

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[3][1], d1stringBCONRotaryItem);
	else
		strcpy(g_szStageVars[3][1], d1MLstringBCONRotaryItem);

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[4][1], d1stringBCONPiercingItem);
	else
		strcpy(g_szStageVars[4][1], d1MLstringBCONPiercingItem);

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[5][1], delstringBCONMic1FrontItem);
	else
		strcpy(g_szStageVars[5][1], delMLstringBCONMic1FrontItem);

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[5][2], delstringBCONMic2FrontItem);
	else
		strcpy(g_szStageVars[5][2], delMLstringBCONMic2FrontItem);

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[5][3], delstringBCONMic3FrontItem);  // 3 Mics for stage 5-7
	else
		strcpy(g_szStageVars[5][3], delMLstringBCONMic3FrontItem);  // 3 Mics for stage 5-7

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[6][1], delstringBCONMic1InsideItem);
	else
		strcpy(g_szStageVars[6][1], delMLstringBCONMic1InsideItem);

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[6][2], delstringBCONMic2InsideItem);
	else
		strcpy(g_szStageVars[6][2], delMLstringBCONMic2InsideItem);

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[6][3], delstringBCONMic3InsideItem);  // 3 Mics for stage 5-7
	else
		strcpy(g_szStageVars[6][3], delMLstringBCONMic3InsideItem);  // 3 Mics for stage 5-7

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[7][1], delboolFlagMic1HeatingItem);
	else
		strcpy(g_szStageVars[7][1], delMLboolFlagMic1HeatingItem);

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[7][2], delboolFlagMic2HeatingItem);
	else
		strcpy(g_szStageVars[7][2], delMLboolFlagMic2HeatingItem);

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[7][3], delboolFlagMic3HeatingItem); // 3 Mics for stage 5-7
	else
		strcpy(g_szStageVars[7][3], delMLboolFlagMic3HeatingItem); // 3 Mics for stage 5-7

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[8][1], delstringBCONLane1ChangeItem); // Just 1 variant @ Stage 8
	else
		strcpy(g_szStageVars[8][1], delMLstringBCONLane1ChangeItem); // Just 1 variant @ Stage 8

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[9][1], delstringBCONLane1EndItem);
	else
		strcpy(g_szStageVars[9][1], delMLstringBCONLane1EndItem);

	if (g_CfgInfo.iPLCType == 0)
		strcpy(g_szStageVars[9][2], delstringBCONLane2EndItem);
	else
		strcpy(g_szStageVars[9][2], delMLstringBCONLane2EndItem);

	// Types - string, bool, etc for each var
	g_szStageTypes[1][1][0] = 's';
	g_szStageTypes[2][1][0] = 's';
	g_szStageTypes[3][1][0] = 's';
	g_szStageTypes[4][1][0] = 's';
	g_szStageTypes[5][1][0] = 's';
	g_szStageTypes[5][2][0] = 's';
	g_szStageTypes[5][3][0] = 's';
	g_szStageTypes[6][1][0] = 's';
	g_szStageTypes[6][2][0] = 's';
	g_szStageTypes[6][3][0] = 's';
	g_szStageTypes[7][1][0] = 'b';
	g_szStageTypes[7][2][0] = 'b';
	g_szStageTypes[7][3][0] = 'b';
	g_szStageTypes[8][1][0] = 's';
	g_szStageTypes[8][2][0] = 's';
	g_szStageTypes[9][1][0] = 's';
	g_szStageTypes[9][2][0] = 's';
} // end PopulateStageVarsAndTypes method, no return value

// Checks items currently dispensing for timeouts
// (from item status list)
// And reports any timeouts to LC
// Also removes items from item-status-list when doing so
void CheckItemsForTimeouts()
{
		// No items dispensing?
		if (!g_iStatusListNodeCount)
			// Nothing to do
			return;

		char szMsg[1024] = {0};
		sprintf(szMsg, "{CheckItemsForTimeouts} Active Dispense Count [%d]", g_iStatusListNodeCount);
		DoLog(szMsg, 5);

		// Get current time
		time_t ttNow;
		time(&ttNow);

		pNode pPrev = NULL;

		// Loop through item-status-list
		for (pNode pIter = g_pHead; pIter != NULL; )
		{
				// Is the time since dispense-start for this item more than timeout ++
				// ..also is the status NOT complete? The status check is just a catch-all for safety
				if ((difftime(ttNow, pIter->PayLoad.ttStartTime) > g_CfgInfo.iDispenseTimeout) && (pIter->PayLoad.iDispenseStage != COMPLETE))
				{
						char szMsg[1024] = {0};
						sprintf(szMsg, "{CheckItemsForTimeouts} Item timeout DispenseID [%s] OrderStub [%s]", pIter->PayLoad.szDispenseID, pIter->PayLoad.szOrderStub);
						DoLog(szMsg, 2);

						// Post timeout to LC
						PostItemStatusToLocalCloud(pIter->PayLoad.szOrderStub, pIter->PayLoad.szDispenseID, TIMEOUT);

						// Purge this item. Yahhhh!
						if (pPrev == NULL)
						{
								// Iter is @ list head, update list head
								g_pHead = pIter->pNext;

								// Delete item
								delete pIter;

								// Decrement
								g_iStatusListNodeCount--;

								// Move iterator to new list head
								pIter = g_pHead;
						} // end of pPrev NULL check
						else
						{
								// Update pPrev's next
								pPrev->pNext = pIter->pNext;

								// Delete item
								delete pIter;

								// Decrement
								g_iStatusListNodeCount--;

								// Move iterator to next node
								pIter = pPrev->pNext;
						} // end of pPrev not-null else
				} // end of difftime greater than timeout block
				else
				{
						// Store the previous item
						pPrev = pIter;

						// Move iterator forward
						pIter = pIter->pNext;
				} // end of difftime not greater than timeout block

		} // end of loop through list
} // end of check items for timeout function, no return value

// This function pings local cloud for new items to dispense
// And returns the list
pItemDispenseData *GetNewItemsFromLocalCloud()
{
	// Default NULL Pointer for return value
	pItemDispenseData *pDispQueue = NULL;

	/// Do a call to LocalCloud to fetch new items json
	// Construct API URL
	char szURL[1024] = {0};
	sprintf(szURL, "http://%s/plcio/order_queue", g_szIPPort);

	char szMsg[1024] = {0};
	sprintf(szMsg, "GetNewItemsFromLocalCloud:: Fetching order queue URL [%s]", szURL);
	DoLog(szMsg, 5);

	// Initialize result struct
	struct MemoryStruct NewItemBuffer = {0};
	NewItemBuffer.pcBuffer = (char *)malloc(1);
	NewItemBuffer.stSize = 0;

fetchURL2:
	// Init easy handle
	CURL *curlEasyHandle = curl_easy_init();

	// This is the URL to fetch
	curl_easy_setopt(curlEasyHandle, CURLOPT_URL, szURL);

	// Timeout 10 seconds
	curl_easy_setopt(curlEasyHandle, CURLOPT_TIMEOUT, 10L);

	// Writer callback function + Writer object
	curl_easy_setopt(curlEasyHandle, CURLOPT_WRITEFUNCTION, CurlWriterCallback);
	curl_easy_setopt(curlEasyHandle, CURLOPT_WRITEDATA, (void *)&NewItemBuffer);

	// Fetch it - this is a blocking call
	CURLcode res = curl_easy_perform(curlEasyHandle);

	// Error check
	if (res != CURLE_OK)
	{
		// Cleanup the handle
		curl_easy_cleanup(curlEasyHandle);

		sprintf(szMsg, "GetNewItemsFromLocalCloud:: Error reading order-queue URL [%s]", curl_easy_strerror(res));
		DoLog(szMsg, 1);

		// Fail
		return NULL;
	}

	// Process the response!
	// Get data from non ASCIIZ buffer
	char *pszResp = new char[NewItemBuffer.stSize + 1];
	memset(pszResp, 0, (NewItemBuffer.stSize + 1) * sizeof(char));
	memcpy(pszResp, NewItemBuffer.pcBuffer, NewItemBuffer.stSize);

	// Cleanup
	free(NewItemBuffer.pcBuffer);

	// Done with easy handle
	curl_easy_cleanup(curlEasyHandle);

	// Try loading into json_t
	json_t *pRoot;
	json_error_t Err;
	pRoot = json_loads(pszResp, 0, &Err);

	// Done with resp buffer
	delete []pszResp;

	// Did we not get a json ptr? Or is it not an array?
	if (!pRoot || !json_is_array(pRoot))
	{
		// Dump error
		char szErr[1024] = {0};
		sprintf(szErr, "GetNewItemsFromLocalCloud:: JSON error [invalid or non array] line %d: [%s] PtrNull: %d\n", Err.line, Err.text, pRoot?1:0);
		DoLog(szErr, 1);

		// Valid json ptr?
		if (pRoot)
		{
			// Dereference json result
			json_decref(pRoot);
		}

		// Bail
		return NULL;
	} // end check for json ptr get

	// Empty array?
	if (json_array_size(pRoot) < 1)
	{
		// Dereference json result
		json_decref(pRoot);

		// Bail
		return NULL;
	}

	// Allocate new items list (size: size of json array + 1) + NULL the ptr array
	// the extra item signals end of queue when iterating it
	pDispQueue = new pItemDispenseData[json_array_size(pRoot) + 1];
	memset(pDispQueue, 0, (json_array_size(pRoot) + 1) * sizeof(pItemDispenseData));

	int iNumItemsStored = 0;

	// Process the array
	for (int i = 0; i < json_array_size(pRoot); i++)
	{
		json_t *pDispenseID, *pStatus, *pOrderStub;

		// Get the i'th row
		json_t *pIter = json_array_get(pRoot, i);

		// Get status and ensure it is string *and* set to "pending"
		pStatus = json_object_get(pIter, "status");
		if (!json_is_string(pStatus) || strcmp(json_string_value(pStatus), "pending"))
		{
			// Skip forward to next array row
			continue;
		}

		// Get dispense ID
		pDispenseID = json_object_get(pIter, "dispense_id");

		// Non-integer?
		if (!json_is_integer(pDispenseID))
		{
			// Skip forward to next array row
			continue;
		}

		// Pre-Check dispense-id against global dispense-id list
		// i.e has this dispense id already begun dispensing?
		int iDispenseID = json_integer_value(pDispenseID);
		if (g_bDispenseIDStarted[iDispenseID]){
			// Skip forward to next array row
			continue;			
		}

		// Get order stub and ensure it is string; len == 49
		pOrderStub = json_object_get(pIter, "order_stub");
		if (!json_is_string(pOrderStub) || (strlen(json_string_value(pOrderStub)) != 59))
		{
			// Skip forward to next array row
			continue;
		}

		// Allocate new item data and NULL it
		pDispQueue[iNumItemsStored] = new ItemDispenseData;
		memset(pDispQueue[iNumItemsStored], 0, sizeof(ItemDispenseData));

		// Populate it
		sprintf(pDispQueue[iNumItemsStored]->szDispenseID, "%lld", json_integer_value(pDispenseID));
		strcpy(pDispQueue[iNumItemsStored]->szOrderStub, json_string_value(pOrderStub));

		char szMsg[1024] = {0};
		sprintf(szMsg, "Got New Item DispenseID: [%s] OrderStub: [%s]",
	 			pDispQueue[iNumItemsStored]->szDispenseID, pDispQueue[iNumItemsStored]->szOrderStub);
		DoLog(szMsg, 2);

		// Increment # of items stored in dispense queue
		iNumItemsStored++;
	} // end loop through json array

	// Dereference json result
	json_decref(pRoot);

	// Return result
	return pDispQueue;
} // end of get-new-items-fromlocalcloud func

// This function asks PLC to dispense the passed item
// Params: pItem - ptr to item dispense struct
void DispenseItemFromList(pItemDispenseData pItem)
{
		// Get barcode from order stub [chars 3 to 26 = 24 chars]
		// String is 0-indexed so we use starting index 2 and # of chars = 24
		char *pszBarCode = substr(pItem->szOrderStub, 2, 34);

		char szMsg[1024] = {0};
		sprintf(szMsg, "DispenseLoop:: SendItem - sending [%s] barcode from dispenser", pszBarCode);
		DoLog(szMsg, 1);

		// Duplicate stub for debugging (with heating flag disabled)
		char szStub[1024] = {0};
		strcpy(szStub, pItem->szOrderStub);

#ifdef ___HEATONLYDUMMY___ // Only for testing purposes, disable heating for non-dummy items!
		if (!strstr(szStub, "TST"))
			// Set heating flag to N
			szStub[26] = 'N';
#endif
		/// Ask PLC to dispense this item
		/// Write the order stub to PLC
		WriteVarToPLC(g_pOrderPLC, g_CompInfo.szOrderVar, szStub, strlen(pItem->szOrderStub));

		// Post status to local cloud - dispense started for this order stub
		// ...Local Cloud will extract dispense id + daily bill number from the stub
		PostItemStatusToLocalCloud(pItem->szOrderStub, pItem->szDispenseID, STARTED);

		/// Append this item to item-status-list with status 'started'
		// Add to item-status-list
		InsertListNode(pItem->szDispenseID, STARTED, pItem->szOrderStub);

		// Set flag for this dispense id 
		g_bDispenseIDStarted[atoi(pItem->szDispenseID)] = TRUE;

		// Cleanup
		delete []pszBarCode;
} // end function streams new items to dispenser compartments [void, no return value]

// Checks item-status-list for the item with supplied dispense-id
// ..and removes it from item-status-list
void PurgeItemFromStatusList(char *pszDispenseID)
{
		pNode pPrev = NULL;

		// Iterate through list which is organized in asc order of dispenseid
		// ..i.e earlier items first
		for (pNode pIter = g_pHead; pIter != NULL;)
		{
				// Dispense ID matches passed string?
				if (atoi(pIter->PayLoad.szDispenseID) == atoi(pszDispenseID))
				{
						/// Need to remove this node from list
						// Edge case 1: This is the head
						if (pIter == g_pHead)
						{
								// Move head fwd
								g_pHead = g_pHead->pNext;

								// Cleanup
								delete pIter;

								// Decrement list count
								g_iStatusListNodeCount--;

								// Move pIter
								pIter = g_pHead;
						}
						else
						{
								/// Non-head node, just delete it
								// Update prev's next-ptr to point to next nodes ptr
								pPrev->pNext = pIter->pNext;

								// Cleanup
								delete pIter;

								// Decrement list count
								g_iStatusListNodeCount--;

								// Move fwd [no change to prev, it remains the prev node]
								pIter = pPrev->pNext;
						}

						// Done
						break;
				} // end check if dispense id matches
				// No, just loop fwd
				else
				{
						// Store prev node
						pPrev = pIter;

						// Move fwd
						pIter = pIter->pNext;
				} // end else block - dispense id does not match
		} // end for-loop iterating through nodes
} // end function to purge item from item-status-list, no return value

// Params:  char string order stub, char string dispense id, STATUS integer, [optional] timer string
void PostItemStatusToLocalCloud(char *pszOrderStub, char *pszDispenseID, int iStatus, char *pszTimerString)
{
	ItemStatusData *pItemData = new ItemStatusData;
	memset(pItemData, 0, sizeof(ItemStatusData));
	strcpy(pItemData->szOrderStub, pszOrderStub);
	strcpy(pItemData->szDispenseID, pszDispenseID);
	pItemData->iStatus = iStatus;
	if (pszTimerString)
		strcpy(pItemData->szTimerString, pszTimerString);

	pthread_t tData;

	char szMsg[1024];
	// Spawn our worker thread
	int iRes = pthread_create(&tData, NULL, &PostItemStatusWorker, (void *)pItemData);
	if (iRes != 0)
	{
		sprintf(szMsg, "PostItemStatusWorker:: Error [%d] spawning Worker Thread", iRes);

		DoLog(szMsg, 2);
	}
	else
	{
		sprintf(szMsg, "PostItemStatusWorker:: Spawned Worker Thread");
		DoLog(szMsg, 2);

		// Detach the thread so it auto-cleans up when done
		pthread_detach(tData);
	}
} // End of PostItemStatusToLocalCloud no return value

// Posts [STARTED/COMPLETE] status of item dispense to local cloud
// Params: void * struct containing char string order stub, char string dispense id, STATUS integer, [optional] timer string
void *PostItemStatusWorker(void *pData)
{
	ItemStatusData *pItemData = (ItemStatusData *) pData;
	char *pszOrderStub = (pItemData->szOrderStub);
	char *pszDispenseID = (pItemData->szDispenseID);
	int iStatus = pItemData->iStatus;
	char *pszTimerString = NULL;
	if(pItemData->szTimerString[0] != '\0')
		pszTimerString = pItemData->szTimerString;

	char szURL[1024] = {0};
	sprintf(szURL, "http://%s/plcio/update_order_item_status", g_szIPPort);
	char szMsg[1024];
	sprintf(szMsg, "PostItemStatusWorkerXXXX:: POSTing id [%s] status [%d] to URL [%s]", pszDispenseID, iStatus, szURL);
	DoLog(szMsg, 2);

	// Initialize result struct
	struct MemoryStruct CfgBuffer = {0};
	CfgBuffer.pcBuffer = (char *)malloc(1);
	CfgBuffer.stSize = 0;

	// Prepare POST body - JSON array
	char szFmtString[] = "{\"data\":{\"dispense_id\":%d,\"status\":\"%s\",\"order_stub\":\"%s\"}}";
	char szData[4096] = {0};
	sprintf(szData, szFmtString, atoi(pszDispenseID), iStatus == STARTED?"dispensing":(iStatus == TIMEOUT?"timeout":"delivered"), pszOrderStub);

	DoLog("Item Status::", 5);
	DoLog(szData, 5);
fetchURLPISTLC:
	// Init easy handle
	CURL *curlEasyHandle = curl_easy_init();

	// This is the URL to fetch
	curl_easy_setopt(curlEasyHandle, CURLOPT_URL, szURL);

	// Timeout 10 seconds
	curl_easy_setopt(curlEasyHandle, CURLOPT_TIMEOUT, 10L);

	// Writer callback function + Writer object
	curl_easy_setopt(curlEasyHandle, CURLOPT_WRITEFUNCTION, CurlWriterCallback);
	curl_easy_setopt(curlEasyHandle, CURLOPT_WRITEDATA, (void *)&CfgBuffer);

	// JSON request header setup
	struct curl_slist *pHdrList = NULL;
	pHdrList = curl_slist_append(pHdrList, "Content-Type: application/json");

	// POST request - hardcoded data string
	curl_easy_setopt(curlEasyHandle, CURLOPT_POST, 1);
	curl_easy_setopt(curlEasyHandle, CURLOPT_POSTFIELDS, szData);
	curl_easy_setopt(curlEasyHandle, CURLOPT_POSTFIELDSIZE, strlen(szData));
	curl_easy_setopt(curlEasyHandle, CURLOPT_HTTPHEADER, pHdrList);

	// Fetch it - this is a blocking call
	CURLcode res = curl_easy_perform(curlEasyHandle);

	// Free our header list
	curl_slist_free_all(pHdrList);

	// Error check
	if (res != CURLE_OK)
	{
		// Cleanup the handle
		curl_easy_cleanup(curlEasyHandle);

		sprintf(szMsg, "PostItemStatusWorker:: Error sending data to LocalCloud [%s], retrying in 5 seconds", curl_easy_strerror(res));
		DoLog(szMsg, 1);

		// Sleep 5 seconds
		sleep(5);

		// Reset buffer
		free(CfgBuffer.pcBuffer);
		CfgBuffer.pcBuffer = (char *)malloc(1);
		CfgBuffer.stSize = 0;

		// Retry entire procedure
		goto fetchURLPISTLC;
	}

	/// Nothing more to be done, the LocalCloud will process the signal
	// Cleanup
	free(CfgBuffer.pcBuffer);

	// Done with easy handle
	curl_easy_cleanup(curlEasyHandle);

	DoLog("PostItemStatusWorker:: Posted item status", 2);

	// Cleanup passed structure
	delete pItemData;

} // end function to post dispense status to local cloud, no return value



// Initialize Compartment Info
// ..using configuration info for this Outlet
void InitializeCompartmentInfo()
{
	/// Only ONE Compartment - the entire dispenser
	pCompartmentInfo pCurr = &g_CompInfo;

	/// Set up the compartment
	// Slot count
	pCurr->iSlotCount = g_CfgInfo.iSlotCount;

	// ControlLogix PLC?
	if (g_CfgInfo.iPLCType == 0)
	{
		// Order-Send Variables
		strcpy(pCurr->szOrderVar, d1stringPlaceOrder);

		// Scan signal variables
		strcpy(pCurr->szDoorClosedVar, d1boolDoorClosed);
		strcpy(pCurr->szOKToOpenDoorVar, d1boolOKToOpenDoor);

		// The readiness variable tells us about dispenser readiness state
		strcpy(pCurr->szDispenseReadinessVar, d1boolReadyForOrdering);

		// Scan start signal - true when scan has started
		strcpy(pCurr->szScanStartVar, d1boolScanStarted);

		// Sync mode :: scan data
		strcpy(pCurr->szSyncBarCodeSlotNumberVar, d1stringsyncBCSlot);

		// Sync mode :: scan complete bit
		strcpy(pCurr->szSyncScanCompleteVar, d1boolsyncScanCompleted);

		// Async mode :: scanned array
		strcpy(pCurr->szAsyncBarCodeArrayVar, d1asyncScannedBarcode);

		// Async mode :: Scan complete bit
		strcpy(pCurr->szAsyncScanCompleteVar, d1boolasyncScanCompleted);
	} // end if ControlLogix check
	else
	{
		// Order-Send Variables
		strcpy(pCurr->szOrderVar, d1MLstringPlaceOrder);

		// Scan signal variables
		strcpy(pCurr->szDoorClosedVar, d1MLboolDoorClosed);
		strcpy(pCurr->szOKToOpenDoorVar, d1MLboolOKToOpenDoor);

		// Scan start signal - true when scan has started
		strcpy(pCurr->szScanStartVar, d1MLboolScanStarted);

		// The readiness variable tells us about dispenser readiness state
		strcpy(pCurr->szDispenseReadinessVar, d1MLboolReadyForOrdering);

		// Sync mode :: scan data
		strcpy(pCurr->szSyncBarCodeSlotNumberVar, d1MLstringsyncBCSlot);

		// Sync mode :: scan complete bit
		strcpy(pCurr->szSyncScanCompleteVar, d1MLboolsyncScanCompleted);
	} // end else if not controllogix (MicroLogix) check
}

// Waits until PLC has ALWAYS ON signalled
// ..noting POWER ON status along the way
// params: PLC fd of target PLC to wait for
void WaitTillPLCReady(PLC *pPLC)
{
	BOOL bPowerON, bAlwaysON = FALSE;

	// Until AlwaysOn
	while (!bAlwaysON)
	{
		// Reset variables
		bPowerON = bAlwaysON = FALSE;

		// Read PowerON state from PLC
		char *pszPowerON;
		if (g_CfgInfo.iPLCType == 0)
		 	pszPowerON = ReadVarFromPLC(pPLC, gboolPLCPowerON, 'b');
		else
			pszPowerON = ReadVarFromPLC(pPLC, gMLboolPLCPowerON, 'b');

		// Read AlwaysON state from PLC
		char *pszAlwaysON;
		if (g_CfgInfo.iPLCType == 0)
			pszAlwaysON = ReadVarFromPLC(pPLC, gboolPLCAlwaysON, 'b');
		else
			pszAlwaysON = ReadVarFromPLC(pPLC, gMLboolPLCAlwaysON, 'b');

		// Did we get some info?
		if (pszPowerON)
		{
			// Was it a 1?
			if (pszPowerON[0] == '1')
				// Signal Power ON flag
				bPowerON = TRUE;

			// Cleanup memory
			delete []pszPowerON;
		} // end of was info provided check

		// Did we get some info on 2nd read?
		if (pszAlwaysON)
		{
			// Was it a 1?
			if (pszAlwaysON[0] == '1')
				// Signal Always ON flag
				bAlwaysON = TRUE;

			// Cleanup memory
			delete []pszAlwaysON;
		} // end of 2nd was info provided check

		// Not Always on?
		if (!bAlwaysON)
		{
			char szMsg[1024] = {0};
			sprintf(szMsg, "WaitForAlwaysON:: Power On State: %d", bPowerON);
			DoLog(szMsg, 2);

			// Sleep 1 second to avoid locking CPU
			sleep(1);
		} // end of if always on check
	} // end of loop waiting for always on
} // void function no return value


// Reads Machine State Data from PLC
// ..and updates item-status-list accordingly with any changes to item status
// ..and signals LocalCloud if any items COMPLETE dispensing
// [this is done by looping through all stages and seeing which item
// .. is at each stage, and updating that item's status accordingly
// .. if it's status in list is an earlier stage]
void ProcessMachineStateData()
{
	// Do we have no items to check?
	if (!g_iStatusListNodeCount)
		// Nothing to do
		return;

	char szMsg[1024] = {0};
	sprintf(szMsg, "{ProcessMachineStateData} Active dispense count [%d]", g_iStatusListNodeCount);
	DoLog(szMsg, 5);

	/// Loop through every stage-variable [1-base index for stages not 0]
	// Stage 1 to MACHINESTAGECOUNT

	for (int i = 1; i <= MACHINESTAGECOUNT; i++)
	{
		// Variant loop [this is basically forks in delivery process
		// .. - like dispenser 1/2, microwave 1/2/3, etc]
		// Each fork is for efficiency, so they are equivalent in terms of stage
		// Stages 1-4 have just 1 fork, as does stage 8
		// Stages 5-7 have 3 forks
		// Stage 9 has 2 forks
		int iVariants;
		if (i < 5 || i == 8)
			iVariants = 1;
		else if (i < 8)
			iVariants = 3;
		else
			iVariants = 2;


		// Loop through the variants
		for (int j = 1; j <= iVariants; j++)
		{
			char cType;
			// Get variable type from substring search of variable name
			// ...has to have string, bool in it - else is int
			// printf("Stage Var: [%i][%j]: %s\n", g_szStageVars[i][j]);
			cType = g_szStageTypes[i][j][0];

			// Read stage-vars for this variant [1 or 2 of them]
			char *pszDataVar1 = NULL;

			// Read only non empty vars (MicroLogix may have some absent)
			// ...and they will be represented as empty space
			if (g_szStageVars[i][j][0] != ' ')
			 	pszDataVar1 = ReadVarFromPLC(g_pOrderPLC, g_szStageVars[i][j], cType);

		 	// No data?
		 	if (!pszDataVar1)
		 		// Iterate forward in loop
		 		continue;

			// Heating (Stage7) is the only stage which doesnt provide us with BCON
			// .. [BarCode-OrderNumber]
			if (i == STAGE7)
			{
				/// Dont fetch DispenseID, we don't have BCON
				// Check flag - is this microwave not heating yet?
				if (!strcmp(pszDataVar1, "1"))
				{
					// Cleanup
					delete []pszDataVar1;

					// Nothing to do
					continue;
				}
				/// This microwave is heating, need to figure out which item is being heated
				// Trawl through item-status-list, find item with this variant [j]
				// ..and status = STAGE6 [just previous stage]
				// ..and update the status of that item to 'Heating' i.e STAGE7
				for (pNode pIter = g_pHead; pIter != NULL; pIter = pIter->pNext)
				{
					// Check current item
					if ((pIter->PayLoad.iDispenseStage == STAGE6) && \
						(pIter->PayLoad.iVariant == j))
					{
						// Update Stage
						pIter->PayLoad.iDispenseStage = STAGE7;

						// Log
						char szMsg[1024] = {0};
						sprintf(szMsg, "ProcessMachineStateData:: Updated DispenseID [%s] to Stage %d Variant %d", \
						 pIter->PayLoad.szDispenseID, STAGE7, j);
						DoLog(szMsg, 4);

						// Exit Loop
						break;
					} // end of check current item
				}	// end of pIter loop
			} // end of if-block [stage7]
			else
			{
				// Get Dispense ID from stage-var1 : char #24 to 33 [10 chars]
				// ...this var holds BarCode [24 chars], Dispense ID [10 chars], and maybe Slot [3 chars]
				char *pszDispenseID = &pszDataVar1[34];

				// NOTE: [[we have asked for slot to be added, no certainty yet - Aug 7, 2015]]
				// NOTE 2016 Jan: Slot won't be reported - pity? Currently we dont need it anyway

				// Truncate here [as the 10 chars are just numeric digits] Ignore the slot for now
				// ..even if slot gets overwritten its no huge loss to us
				pszDispenseID[6] = '\0';


				// Update item-status-list for this item if stage is later than item status currently
				// Trawl through item-status-list, find item with this dispenseid
				// ..and check the status
				// DoLog("ProcessMachineStateData:: Checking ISL for matches\n");
				for (pNode pIter = g_pHead; pIter != NULL; pIter = pIter->pNext)
				{
					char szMsg2[1024] = {0};
					sprintf(szMsg2, "ProcessMachineStateData:: Comparing dispense id [%s] to ISL Item %s Stage %d\n", pszDispenseID, pIter->PayLoad.szDispenseID, pIter->PayLoad.iDispenseStage);
					DoLog(szMsg2, 6);

					// Does current DispenseID match passed Dispense ID?
					if (atoi(pIter->PayLoad.szDispenseID) == atoi(pszDispenseID))
					{
						/// Yes, check status
						// Was last recorded stage less than the current stage got from PLC?
						if (pIter->PayLoad.iDispenseStage < i)
						{
							// Update the stage
							pIter->PayLoad.iDispenseStage = i;

							// Update the variant - e.g dispenser2 can goto mic1 to lane2, etc
							// so variant would be 2 then 1 then 2 in the e.g
							pIter->PayLoad.iVariant = j;

							/// Time this stage
							char szTimerData[1024];
							time_t ttTimeNow;

							// Get time_t value (# of seconds since EPOCH)
							time(&ttTimeNow);

							// Build timer string segment to add to string
							sprintf(szTimerData, "%d:%ld|", i, ttTimeNow);

							// Do we already have data in the string?
							if (pIter->PayLoad.szTimerString[0])
								// Append to end of existing string
								strcat(pIter->PayLoad.szTimerString, szTimerData);
							// No existing timer-data so far
							else
								// Begin string with this timer string-segment
							 	strcpy(pIter->PayLoad.szTimerString, szTimerData);

							char szMsg[1024] = {0};
							sprintf(szMsg, "ProcessMachineStateDataxxxx:: Updated DispenseID [%s] to Stage %d Variant %d TimerString [%s]", \
							 pIter->PayLoad.szDispenseID, i, j, pIter->PayLoad.szTimerString);
							DoLog(szMsg, 4);

							// Is this an item dispense completion?
							if (i == COMPLETE)
							{
								sprintf(szMsg, "ProcessMachineStateData:: Item Complete! DispenseID: [%s]",\
								 	pIter->PayLoad.szDispenseID);
								DoLog(szMsg, 1);
DoLog("WriteCompletionStatusToFile",1);

								// Write the order # to file so that the machine can display it
								// (also pass the variant == lane number)
								WriteCompletionStatusToFile(pIter->PayLoad.szOrderStub, j);

								// Post status to local cloud [dont remove this item from list here]
								// ..also posting the Timer Data String
								PostItemStatusToLocalCloud(pIter->PayLoad.szOrderStub, pszDispenseID, COMPLETE, pIter->PayLoad.szTimerString);

								// Purge from status list
								PurgeItemFromStatusList(pszDispenseID);
							}
						} // end status check

						// Done with loop, we found the right dispenseID!
						break;
					} // end dispenseID check
				} // end pNode iter loop
			} // end else case [not STAGE7]

			// Cleanup strings we read
			delete []pszDataVar1;

		} // end j loop
	} // end i loop
} // End ProcessMachineStateData functon, no return value

// Writes order # of completed item to Lane1.txt or Lane2.txt in 
// the /home/ubuntu folder
// Params: order stub (49 char) used to find lane# and order#
void WriteCompletionStatusToFile(char *pszOrderStub, int iLane)
{
	// Construct filename to write
	char szFileName[1024] = {0};
	sprintf(szFileName, "/home/ubuntu/OrderAtLane%d.txt", iLane);

	// Open as text file for writing to
	FILE *ptr = fopen(szFileName, "wt");

	// Valid file ptr?
	if (ptr)
	{
		char szStubDupe[100] = {0};

		// Duplicate order stub
		strcpy(szStubDupe, pszOrderStub);
DoLog(pszOrderStub,1);
		// Get order number pointer
		char *pszOrderNum = &szStubDupe[51];

		// Convert to integer
		int iOrderNum = atoi(pszOrderNum);
DoLog(pszOrderNum ,1);
		// Write to file
		fprintf(ptr, "%d", iOrderNum);

		// Flush buffer
		fflush(ptr);

		// Close file
		fclose(ptr);
	} // end valid file ptr check

	// Done
} // End of write completion status function, no return value


// Scan worker function
// ..this is spawned at Service startup
// ..and remains active, checking for a machine-scan
// ..handling both sync and async cases
// params: pArg = NULL (no argument needs to be passed)
void *ScanWorkerFunction(void *pArg)
{
	/// Sleep a bit before we begin Processing
	/// ...This allows the other PLC connection to go through without
	/// ...running into blocking operations from the PLC
	sleep(5); // 5 seconds

	// Connect to PLC (for scan detection, solicited mode)
	// USED FOR BOTH SYNC ANC ASYNC CASES
	g_pScanPLC = ConnectToPLC(g_CfgInfo.szPLCIP, g_CfgInfo.iPLCPort, g_CfgInfo.iPLCType == 1);

	DoLog("ScanWorker:: Connected to PLC for Scan Processing");

	// Is this an Async Scan PLC?
	if(g_CfgInfo.bAsyncScan)
	{
		DoLog("ScanWorker:: Entered Async Scan Loop");

		// Loop until app done
		while (!g_bAppDone)
		{
				// Get scan status from the PLC
				BOOL bScanStatus = GetScanStatus(g_pScanPLC);

				// Is a scan in progress?
				if (bScanStatus)
				{
						// Log scan start
						char szMsg[1024];
						sprintf(szMsg, "ScanWorker:: Async Scan started!");
						DoLog(szMsg);

						// Inform local cloud that scan has started
						pthread_create(&scanThreadID, NULL, &SendScanStartSignalToLocalCloud, NULL);

						/// Check scan completion bit and loop until it is set
						// Loop forever - this breaks from within
						while (TRUE)
						{
								// Read async scan completion variable from PLC
								char *pszScanComplete = ReadVarFromPLC(g_pScanPLC, g_CompInfo.szAsyncScanCompleteVar, 'b');

								// Did we get a result?
								if (pszScanComplete)
								{
										// Scan complete bit set to 1?
										if (!strcmp(pszScanComplete, "1"))
										{
												// Cleanup
												delete []pszScanComplete;

												// Exit WHILE loop
												break;
										}
								} // End did we get result check

								// Cleanup
								delete []pszScanComplete;

								// Sleep half a second = 500K microseconds to avoid hogging CPU
								usleep(0.5 * 1000000);
						}

						/// Scan is complete, we need to process this compartment
						char szScannedBarCodeArray[MAXITEMS][35] = {0};
						char szScannedSlotArray[MAXITEMS][10] = {0};
						int iNumScannedItems;

						// NULL the arrays for this loop iteration
						memset(szScannedSlotArray, 0, sizeof(szScannedSlotArray));
						memset(szScannedBarCodeArray, 0, sizeof(szScannedBarCodeArray));
						iNumScannedItems = 0;

						// Iterate through slots
						for (int iIdx = 0; iIdx < g_CompInfo.iSlotCount; iIdx++)
						{
								/// Does this slot have a barcode?
								// Construct varname - we have to read an ARRAY
								// ..so the varnames are X[1], X[2], etc.
								char szVarName[1024] = {0};
								sprintf(szVarName, "%s[%d]", g_CompInfo.szAsyncBarCodeArrayVar, iIdx);

								// printf("Reading %s\n", szVarName);

								// Read from PLC
								char *pszBarCode = ReadVarFromPLC(g_pScanPLC, szVarName, 's');

								// No result?
								if(!pszBarCode)
										// Iterate fwd to next slot
										continue;

								// Did we get at-least 24 chars? (Barcode length)
								if (strlen(pszBarCode) > 33)
								{
										char szSlotNumber[10];
										/// Note: in sync case, there may be duplicates of same
										/// {barcode, slot} as we poll just one variable for data
										/// ...But in async case, there are no such issues as we
										/// ...read one variable for each slot, so no de-duping required
										// Convert slot # to string to do comparisons and logging
										sprintf(szSlotNumber, "%d", iIdx);

										// Add to scan results and increment scanned item count
										strcpy(szScannedBarCodeArray[iNumScannedItems], pszBarCode);
										// Safe string copy for the 2nd chunk (slot string) - upto 9 chars
										strncpy(szScannedSlotArray[iNumScannedItems], szSlotNumber, 9 * sizeof(char));
										iNumScannedItems++;

										char szMsg[1024] = {0};
										sprintf(szMsg, "ScanWorker:: Got Async Scan Item: Numitems: %d and extracted [bc: %s slot: %s]", \
															iNumScannedItems, pszBarCode, szSlotNumber);
										DoLog(szMsg, 2);

								} // end valid barcode check
								// Else if we got ANY data (at least 1 char)
								else if (strlen(pszBarCode) > 0)
								{
										char szMsg[1024] = {0};
										sprintf(szMsg, "ScanWorker:: Got Invalid Async scan data [%s]", pszBarCode);
										DoLog(szMsg, 1);
								} // end else [valid barcode slot number check]

								// Done with barcode, cleanup
								delete []pszBarCode;
						} // end loop through slots

						// Update local stock tables
						UpdateDispenserStock(szScannedSlotArray, szScannedBarCodeArray, iNumScannedItems);

						// Post total stock (now modified by scan results) to Local Cloud
						PostTotalStockToLocalCloud();

						// Wait until scan-vars reset by PLC (as they may remain true for a while)
						while (bScanStatus == GetScanStatus(g_pScanPLC))
								// Sleep a while (0.1 second) to avoid hogging CPU
								// = 0.1 x 1M microseconds
								usleep(0.1 * 1000000);

				} // end of scan-in-progress check
		}  // end of async scan process loop
	} // end of async scan handling code
	else
	{
		DoLog("ScanWorker:: Entered Sync Scan Loop");

		// Loop until app done
		while (!g_bAppDone)
		{
			// Get scan status from the PLC
			BOOL bScanStatus = GetScanStatus(g_pScanPLC);

			// Is a scan in progress?
			if (bScanStatus)
			{
				// Log scan start
				char szMsg[1024] = {0};
				sprintf(szMsg, "ScanWorker:: Sync Scan started [signal received]");
				DoLog(szMsg);

				// Inform local cloud that scan has started
				pthread_create(&scanThreadID, NULL, &SendScanStartSignalToLocalCloud, NULL);

				BOOL bDataReceived = FALSE;

				char szScannedBarCodeArray[MAXITEMS][35] = {0};
				char szScannedSlotArray[MAXITEMS][10] = {0};
				int iNumScannedItems;

				// NULL the arrays for this loop iteration
				memset(szScannedSlotArray, 0, sizeof(szScannedSlotArray));
				memset(szScannedBarCodeArray, 0, sizeof(szScannedBarCodeArray));
				iNumScannedItems = 0;

				// Do until num scanned items exceeds max
				// ...this is just a sanity check, the loop will break due
				// ...to other conditions being fulfilled (scancomplete set and no more
				// ...barcodes coming in)
				while (iNumScannedItems < MAXITEMS)
				{
						// Read a barcode + slot number from PLC
						char *pszBarCodeSlotNumber = ReadVarFromPLC(g_pScanPLC, g_CompInfo.szSyncBarCodeSlotNumberVar, 's');

						// Check barcode and slot number strings for
						// ...valid result: i.e non NULL PTR, and string isnt empty?
						if ((pszBarCodeSlotNumber != NULL) && (*pszBarCodeSlotNumber != '\0'))
						{
								// Need atleast 24 chars for barcode and 1 for slot number
								if (strlen(pszBarCodeSlotNumber) >= 35)
								{
										/// OK, this is a valid result string
										if (!bDataReceived)
										{
												// Data received
												bDataReceived = TRUE;

												sprintf(szMsg, "ScanWorker:: Sync Scan [data received]");
												DoLog(szMsg);
										}
										// Extract barcode & slot number
										char *pszBarCode = substr(pszBarCodeSlotNumber, 0, 34);
										char *pszSlotNumber = substr(pszBarCodeSlotNumber, 34, strlen(pszBarCodeSlotNumber) - 34);

										char szMsg1[1024] = {0};
										sprintf(szMsg1, "ScanWorker:: PLC Scan-data: [%s] Items so far: %d; Extracted [bc: %s slot: %s]; Checking if already stored", \
															pszBarCodeSlotNumber, iNumScannedItems, pszBarCode, pszSlotNumber);
										DoLog(szMsg1, 5);

										BOOL bPresent = FALSE;
										/// Non-Duplication of scanned {barcode, slot}: SYNC scan only
										// Do we already have this scan result? i.e. same slot?
										for (int iCheck = 0; iCheck < iNumScannedItems; iCheck++)
										{
											// Match the iterator slot?
											if (!strcmp(szScannedSlotArray[iCheck], pszSlotNumber))
											{
												// Yes, already present
												bPresent = TRUE;

												// Exit loop
												break;
											}
										}

										// Was it not already present?
										if (!bPresent)
										{
											// Add to scan results and increment scanned item count
											strcpy(szScannedBarCodeArray[iNumScannedItems], pszBarCode);
											// Safe string copy for the 2nd chunk (slot string) - upto 9 chars
											strncpy(szScannedSlotArray[iNumScannedItems], pszSlotNumber, 9 * sizeof(char));
											iNumScannedItems++;

											char szMsg[1024] = {0};
											sprintf(szMsg, "ScanWorker:: Got New Item - Scan Data: [%s] Items so far: %d Item [bc: %s slot: %s]", \
																pszBarCodeSlotNumber, iNumScannedItems, pszBarCode, pszSlotNumber);
											DoLog(szMsg, 2);
										}

										// Cleanup
										delete []pszBarCode;
										delete []pszSlotNumber;
								} // end 25+ char barcode-slotnumber check
								else
								{
										char szMsg[1024] = {0};
										sprintf(szMsg, "ScanWorker:: Got Invalid scan data [%25s]", pszBarCodeSlotNumber[0] != '\0' ? pszBarCodeSlotNumber : "NULL");
										DoLog(szMsg, 5);
								} // end else [valid barcode slot number check]

								// Cleanup
								delete []pszBarCodeSlotNumber;

						} // end valid barcode + slotnumber strings check

						/// Has scan been completed?
						// Check if scan complete bit is set
						char *pszScanComplete = ReadVarFromPLC(g_pScanPLC, g_CompInfo.szSyncScanCompleteVar, 'b');

						// Non-Null result?
						if (pszScanComplete)
						{
								// Check if bit is set
								if (!strcmp(pszScanComplete, "1"))
								{
									// Cleanup memory
									delete []pszScanComplete;

									// Exit loop
									break;
								}

								// Cleanup memory
								delete []pszScanComplete;
						} // end of non-null result check

						// Sleep a while to avoid hogging CPU
						// = 0.1 x 1M microseconds
						usleep(0.1 * 1000000);
				} // end of until-scan-complete loop

				// Update local stock tables
				UpdateDispenserStock(szScannedSlotArray, szScannedBarCodeArray, iNumScannedItems);

				// Post total stock (now modified by scan results) to Local Cloud
				PostTotalStockToLocalCloud();

				int iWait = 0;

				// Wait until scan-vars reset by PLC (as they may remain true for a while)
				while (GetScanStatus(g_pScanPLC))
				{
						// Sleep a while (0.1 second) to avoid hogging CPU
						// = 0.1 x 1M microseconds
						usleep(0.1 * 1000000);

						// Counter
						iWait++;

						// Too long?
						if ((iWait % 20) == 0)
							DoLog("ScanWorker:: Delay waiting for scan signals to reset to 0", 2);
				}

			} // end of if-scan-in-progress block
			// No scan
			else
				// Sleep a while (2.0 second) to avoid hogging CPU
				// = 0.5 x 1M microseconds
				usleep(0.5 * 1000000);

		} // end of sync scan-wait-and-process loop
	}

	/// Close PLC Connections
	// Scan PLC
	DisconnectFromPLC(g_pScanPLC);

	// Done
	return NULL;
} // end scan worker function

// Checks dispenser for scan activity
// Also: Checks for wipe-off and sets wipe off flag if wipe-off done
// Param: PLC fd
// Returns: TRUE if scan in progress, FALSE otherwise
BOOL GetScanStatus(PLC *pPLC)
{
		/// First check scan started var
		char *pszScanStarted = ReadVarFromPLC(pPLC, g_CompInfo.szScanStartVar, 'b');

		// Did we get a result? And is it TRUE (1) ?
		if (pszScanStarted && !strcmp(pszScanStarted, "1"))
		{
				// Cleanup
				delete []pszScanStarted;

				// Success!
				return TRUE;
		} // end scan started result present & true check

		// Cleanup
		if (pszScanStarted)
				delete []pszScanStarted;

		// Is the wipe-off already signalled? No need to check again if so
		if (g_bWipeOffDone)
				// Scan not started, but we can bypass the check
				return FALSE;

		/// Wipe-Off Check [as Scan not started]
		/// We have two vars to check for each compartment:
		/// (a) Door Closed == FALSE and
		/// (b) OK to open door == TRUE
		// Door Closed = FALSE?
	  	char *pszDoorClosed = ReadVarFromPLC(pPLC, g_CompInfo.szDoorClosedVar, 'b');
		char szMsg[1024] = {0};
		sprintf(szMsg, "GetScanStatus:: DoorClosed [%s]", pszDoorClosed);
		DoLog(szMsg, 6);

		// Has the door been opened?
		if (pszDoorClosed && !strcmp(pszDoorClosed, "0"))
		{
				// Read OK to open door
				char *pszOKToOpenDoor = ReadVarFromPLC(pPLC, g_CompInfo.szOKToOpenDoorVar, 'b');

				sprintf(szMsg, "GetScanStatus:: OKToOpenDoor [%s]", pszOKToOpenDoor);
				DoLog(szMsg, 5);

				// Is it OK to open door?
				if (pszOKToOpenDoor && !strcmp(pszOKToOpenDoor, "1"))
				{
						// Wipe off has been done
						g_bWipeOffDone = TRUE;

						sprintf(szMsg, "GetScanStatus:: Wipe-Off done");
						DoLog(szMsg, 4);

						// Submit stock to local cloud with wipeoff = TRUE
						g_iBarCodeCount = 0;
						PostTotalStockToLocalCloud();
				} // end check if result present & ok to open door

				// Cleanup
				if (pszOKToOpenDoor)
					delete []pszOKToOpenDoor;
		} // end check if result present & door closed

		// Cleanup
		if (pszDoorClosed)
			delete []pszDoorClosed;

		// Default, no scan
		return FALSE;
} // end get-scan-status (+ set wipe-off status) function

// This function gets scanned results of a single compartment scan
// ...in arrays {barcode}, {slot}, and builds {barcode, slotstring} arrays
// ...slotstring is just combination of all slots for 1 barcode into a string
// ...(then overwrites global stock table for that compartment with the new data)
// Params: container #, scanned slots array, scanned barcodes array, num items scanned
void UpdateDispenserStock(char pszSlotArray[][10], char pszBarCodeArray[][35], int iNumScanned)
{
		char szMsg[1024] = {0};
		sprintf(szMsg, "UpdateStock:: NumScanned %d", iNumScanned);
		DoLog(szMsg, 2);

		// Lock the stock table mutex
		pthread_mutex_lock(&g_stockLock);

		/// We need to process the stock array and convert it into a form
		/// ..that can be stored
		/// Our global stock array: {Barcode, SlotString, Qty}
		g_iBarCodeCount = 0;

		// Loop through each scanned item
		for (int iLoop = 0; iLoop < iNumScanned; iLoop++)
		{
				// Check if this barcode is already in array
				int iLoop2;
				for (iLoop2 = 0; iLoop2 < g_iBarCodeCount; iLoop2++)
				{
						// Try to string match barcodes
						if (!strcmp(pszBarCodeArray[iLoop], g_szBarCodeArray[iLoop2]))
						{
								/// We have a match
								// Append separator (comma) and slot number to slot string for this barcode
								strcat(g_szSlotStringArray[iLoop2], ",");
								strcat(g_szSlotStringArray[iLoop2], pszSlotArray[iLoop]);
								g_iSlotCountArray[iLoop2]++;

								sprintf(szMsg, "UpdateStock:: Got BarCode [%s] New Slot %s",
								  g_szBarCodeArray[iLoop2], pszSlotArray[iLoop]);
								DoLog(szMsg, 2);

								// Done with loop
								break;
						}
				}

				// Did we pass loop end? ie was barcode not found ?
				if (iLoop2 == g_iBarCodeCount)
				{
						// Yes, add this item to stock table for this container, and increment item count
						strcpy(g_szBarCodeArray[g_iBarCodeCount], pszBarCodeArray[iLoop]);
						strcpy(g_szSlotStringArray[g_iBarCodeCount], pszSlotArray[iLoop]);
						g_iSlotCountArray[g_iBarCodeCount] = 1;
						g_iBarCodeCount++;

						sprintf(szMsg, "UpdateStock:: Got new BarCode [%s]", \
							g_szBarCodeArray[g_iBarCodeCount - 1]);
						DoLog(szMsg, 2);
				}
		} // end scanned-item loop

		// Unlock the stock table mutex
		pthread_mutex_unlock(&g_stockLock);

		sprintf(szMsg, "UpdateStock:: Got %d BarCodes", g_iBarCodeCount);
		DoLog(szMsg, 2);

		// Log each barcode
		for (int iL = 0; iL < g_iBarCodeCount; iL++)
		{
			sprintf(szMsg, "UpdateStock:: Barcode [%s] Slots [%s]\n", g_szBarCodeArray[iL], g_szSlotStringArray[iL]);
			DoLog(szMsg, 2);
		}
}

// This function does a HTTP POST to Local Cloud
// Passing the barcode/slot arrays to local cloud
// To local Cloud
void PostTotalStockToLocalCloud()
{
	char szURL[1024] = {0};
	sprintf(szURL, "http://%s/plcio/submit_scanned_stock", g_szIPPort);
	char szMsg[1024];
	sprintf(szMsg, "PostTotalStockToLocalCloud:: POSTing data to URL [%s]", szURL);
	DoLog(szMsg, 2);

	// Initialize result struct
	struct MemoryStruct CfgBuffer = {0};
	CfgBuffer.pcBuffer = (char *)malloc(1);
	CfgBuffer.stSize = 0;

	// Prepare POST body - json array
	char szFmtString[] = "{\"data\":[%s], \"append_only\": %s}";
	char szData[MAXITEMS * 105] = {0};
	char szRows[MAXITEMS * 100] = {0};

	// Lock the stock table mutex
	pthread_mutex_lock(&g_stockLock);

	// Loop through stock table - each row key is 1 barcode
	for (int iL = 0; iL < g_iBarCodeCount; iL++)
	{
			char szRow[1024];
			memset(szRow, 1024 * sizeof(char), 0);
			sprintf(szRow, "{\"barcode\":\"%s\",\"count\":%d,\"slot_ids\":\"%s\"}", \
				g_szBarCodeArray[iL], g_iSlotCountArray[iL], g_szSlotStringArray[iL]);

			// Append comma if not at end
			if (iL < (g_iBarCodeCount - 1))
				strcat(szRow, ", ");

			// Append to our rows array
			if (szRows[0] != '\0')
				strcat(szRows, szRow);
			else
				strcpy(szRows, szRow);
	} // end loop through stock table

	// Unlock the stock table mutex
	pthread_mutex_unlock(&g_stockLock);

	// Append Only = No Wipe off done. Append Only False == Wipe Off Done
	// Build final POST body string
	sprintf(szData, szFmtString, szRows[0] != '\0' ? szRows:" ", g_bWipeOffDone ? "false": "true");
	DoLog("Scan Data::", 5);
	DoLog(szData, 5);

fetchURLPTSTLC:
	// Init easy handle
	CURL *curlEasyHandle = curl_easy_init();

	// This is the URL to fetch
	curl_easy_setopt(curlEasyHandle, CURLOPT_URL, szURL);

	// Timeout 10 seconds
	curl_easy_setopt(curlEasyHandle, CURLOPT_TIMEOUT, 10L);

	// Writer callback function + Writer object
	curl_easy_setopt(curlEasyHandle, CURLOPT_WRITEFUNCTION, CurlWriterCallback);
	curl_easy_setopt(curlEasyHandle, CURLOPT_WRITEDATA, (void *)&CfgBuffer);

	// JSON request header setup
	struct curl_slist *pHdrList = NULL;
	pHdrList = curl_slist_append(pHdrList, "Content-Type: application/json");

	// POST request - hardcoded data string
	curl_easy_setopt(curlEasyHandle, CURLOPT_POST, 1);
	curl_easy_setopt(curlEasyHandle, CURLOPT_POSTFIELDS, szData);
	curl_easy_setopt(curlEasyHandle, CURLOPT_POSTFIELDSIZE, strlen(szData));
	curl_easy_setopt(curlEasyHandle, CURLOPT_HTTPHEADER, pHdrList);

	// Fetch it - this is a blocking call
	CURLcode res = curl_easy_perform(curlEasyHandle);

	// Free our header list
	curl_slist_free_all(pHdrList);

	// Error check
	if (res != CURLE_OK)
	{
		// Cleanup the handle
		curl_easy_cleanup(curlEasyHandle);

		sprintf(szMsg, "PostTotalStockToLocalCloud:: Error sending data to LocalCloud [%s], retrying in 5 seconds", curl_easy_strerror(res));
		DoLog(szMsg, 1);

		// Sleep 5 seconds
		sleep(5);

		// Reset buffer
		free(CfgBuffer.pcBuffer);
		CfgBuffer.pcBuffer = (char *)malloc(1);
		CfgBuffer.stSize = 0;

		// Retry entire procedure
		goto fetchURLPTSTLC;
	}

	/// Nothing more to be done, the LocalCloud will process the signal
	// Cleanup
	free(CfgBuffer.pcBuffer);

	// Done with easy handle
	curl_easy_cleanup(curlEasyHandle);

	// Reset wipe-off done variable - only if this was a regular submit (else
	// ..the system will keep posting wipe-offs to local cloud)
	if (g_iBarCodeCount > 0)
		g_bWipeOffDone = FALSE;

	DoLog("PostTotalStockToLocalCloud:: Posted total stock", 2);


} // end of total stock to local cloud post, no return value

// This function does a HTTP POST to Local Cloud
// ..notifying LocalCloud that a scan has begun @ the Machine
void *SendScanStartSignalToLocalCloud(void *pArg)
{
	char szURL[1024] = {0};
	sprintf(szURL, "http://%s/plcio/dispenser_status", g_szIPPort);
	char szMsg[1024];
	sprintf(szMsg, "SendScanStartSignalToLocalCloud:: POSTing data to URL [%s]", szURL);
	DoLog(szMsg, 2);

	// Initialize result struct
	struct MemoryStruct CfgBuffer = {0};
	CfgBuffer.pcBuffer = (char *)malloc(1);
	CfgBuffer.stSize = 0;

	// Post body
	char szData[100] = "{\"status\": \"loading\"}";

fetchURLSSSSTLC:
	// Init easy handle
	CURL *curlEasyHandle = curl_easy_init();

	// This is the URL to fetch
	curl_easy_setopt(curlEasyHandle, CURLOPT_URL, szURL);

	// Timeout 10 seconds
	curl_easy_setopt(curlEasyHandle, CURLOPT_TIMEOUT, 10L);

	// Writer callback function + Writer object
	curl_easy_setopt(curlEasyHandle, CURLOPT_WRITEFUNCTION, CurlWriterCallback);
	curl_easy_setopt(curlEasyHandle, CURLOPT_WRITEDATA, (void *)&CfgBuffer);

	// JSON request header setup
	struct curl_slist *pHdrList = NULL;
	pHdrList = curl_slist_append(pHdrList, "Content-Type: application/json");

	// POST request - hardcoded data string
	curl_easy_setopt(curlEasyHandle, CURLOPT_POST, 1);
	curl_easy_setopt(curlEasyHandle, CURLOPT_POSTFIELDS, szData);
	curl_easy_setopt(curlEasyHandle, CURLOPT_POSTFIELDSIZE, strlen(szData));
	curl_easy_setopt(curlEasyHandle, CURLOPT_HTTPHEADER, pHdrList);

	// Fetch it - this is a blocking call
	CURLcode res = curl_easy_perform(curlEasyHandle);

	// Free our header list
	curl_slist_free_all(pHdrList);

	// Error check
	if (res != CURLE_OK)
	{
		// Cleanup the handle
		curl_easy_cleanup(curlEasyHandle);

		sprintf(szMsg, "SendScanStartSignalToLocalCloud:: Error sending data to LocalCloud [%s], retrying in 5 seconds", curl_easy_strerror(res));
		DoLog(szMsg, 1);

		// Sleep 5 seconds
		sleep(5);

		// Reset buffer
		free(CfgBuffer.pcBuffer);
		CfgBuffer.pcBuffer = (char *)malloc(1);
		CfgBuffer.stSize = 0;

		// Retry entire procedure
		goto fetchURLSSSSTLC;
	}

	/// Nothing more to be done, the LocalCloud will process the signal
	// Cleanup
	free(CfgBuffer.pcBuffer);

	// Done with easy handle
	curl_easy_cleanup(curlEasyHandle);

	DoLog("SendScanStartSignalToLocalCloud:: Posted scan start signal", 2);
} // end of send scan start signal to local cloud, no return value


// This function does a HTTP GET to LocalCloud
// ...to fetch the config info for this outlet
// ...infinitely retrying with delays, until the config info is retrieved
// Params: ConfigInfo struct (overwritten with new config info)
void GetConfigFromLocalCloud(ConfigInfo *pCfgInfo)
{
	/// First fetch LC Server IP+Port from environment
	char *pszIPPort = getenv("LocalCloudServer");
	if (!pszIPPort)
	{
		// Issue
		DoLog("Unable to read 'LocalCloudServer' environment variable!\n");

		// Exit with error
		exit(1);
	}

	// Convert to 21-char string
	sscanf(pszIPPort, "%21s", g_szIPPort);

	// Got nothing?
	if (g_szIPPort[0] == '\0')
	{
		// Issue
		DoLog("Unable to read 'LocalCloudServer' environment variable!\n");

		// Exit with error
		exit(1);
	}

	char szMsg[1024] = {0};
	sprintf(szMsg, "GetConfigFromLocalCloud:: Got Local Cloud IP-Port [%s]", g_szIPPort);
	DoLog(szMsg, 1);

	char szURL[1024] = {0};
	sprintf(szURL, "http://%s/plcio/config", g_szIPPort);
	sprintf(szMsg, "GetConfigFromLocalCloud:: Fetching config URL [%s]", szURL);
	DoLog(szMsg, 1);

	// Initialize result struct
	struct MemoryStruct CfgBuffer = {0};
	CfgBuffer.pcBuffer = (char *)malloc(1);
	CfgBuffer.stSize = 0;

fetchURL:
	// Init easy handle
	CURL *curlEasyHandle = curl_easy_init();

	// This is the URL to fetch
	curl_easy_setopt(curlEasyHandle, CURLOPT_URL, szURL);

	// Timeout 10 seconds
	curl_easy_setopt(curlEasyHandle, CURLOPT_TIMEOUT, 10L);

	// Writer callback function + Writer object
	curl_easy_setopt(curlEasyHandle, CURLOPT_WRITEFUNCTION, CurlWriterCallback);
	curl_easy_setopt(curlEasyHandle, CURLOPT_WRITEDATA, (void *)&CfgBuffer);

	// Fetch it - this is a blocking call
	CURLcode res = curl_easy_perform(curlEasyHandle);

	// Error check
	if (res != CURLE_OK)
	{
		// Cleanup the handle
		curl_easy_cleanup(curlEasyHandle);

		sprintf(szMsg, "GetConfigFromLocalCloud:: Error reading config URL [%s], retrying in 5 seconds", curl_easy_strerror(res));
		DoLog(szMsg, 1);

		// Sleep 5 seconds
		sleep(5);

		// Reset buffer
		free(CfgBuffer.pcBuffer);
		CfgBuffer.pcBuffer = (char *)malloc(1);
		CfgBuffer.stSize = 0;

		// Retry entire procedure
		goto fetchURL;
	}

	// Process the response!
	ProcessCfgResponse(pCfgInfo, &CfgBuffer);

	// Cleanup
	free(CfgBuffer.pcBuffer);

	// Done with easy handle
	curl_easy_cleanup(curlEasyHandle);

	// Check if we got valid data: If not, we need to retry
	// Did we get the PLC IP? And Lower slot count, lane count, dispenser count
	if (!(pCfgInfo->szPLCIP[0] && pCfgInfo->iSlotCount && pCfgInfo->iLaneCount))
	{
		// No, we missed something
		sprintf(szMsg, "GetConfigFromLocalCloud:: Incomplete config data PLCIP [%s] SlotCount %d LaneCount %d"
			, pCfgInfo->szPLCIP, pCfgInfo->iSlotCount, pCfgInfo->iLaneCount);
		DoLog(szMsg, 1);

		// Reset buffer
		CfgBuffer.pcBuffer = (char *)malloc(1);
		CfgBuffer.stSize = 0;

		// Sleep a bit
		sleep(5);

		// Retry
		goto fetchURL;
	}

	/*
	// Hardcoded numbers - CHENNAI SILKS
	pCfgInfo->iLaneCount = 2;
	pCfgInfo->bAsyncScan = TRUE;
	pCfgInfo->iSlotCount = 160;
	strcpy(pCfgInfo->szPLCIP, "192.168.1.80");
	*/
} // void func, no return value

// Curl Writer callback used for getconfig url-fetch
// See CURLOPT_WRITEFUNCTION spec for desc of this function
// Parameters: ReadData, Size, # of items, user-specified struct
static size_t CurlWriterCallback(void *pContents, size_t stSize,
	size_t stNum, void *pUser)
{
	size_t stRealSize = stSize * stNum;
  struct MemoryStruct *pMem = (struct MemoryStruct *)pUser;

  pMem->pcBuffer = (char *)realloc(pMem->pcBuffer, pMem->stSize + stRealSize + 1);

	// Ran out of memory ?
  if(pMem->pcBuffer == NULL)
    // Fail
		return 0;

  memcpy(&(pMem->pcBuffer[pMem->stSize]), pContents, stRealSize);
  pMem->stSize += stRealSize;
  pMem->pcBuffer[pMem->stSize] = 0;

  return stRealSize;
} // end of curl writer callback

// Extracts configuration info from a jsonized response from LocalCloud
// Parameters: ConfigInfo pointer (to write data to), Memory Struct buffer with data to process
void ProcessCfgResponse(ConfigInfo *pCfgInfo, struct MemoryStruct *pData)
{
	// Get data from non ASCIIZ buffer
	char *pszResp = new char[pData->stSize + 1];
	memset(pszResp, 0, (pData->stSize + 1) * sizeof(char));
	memcpy(pszResp, pData->pcBuffer, pData->stSize);

	// Test Test Test
	// pszResp = new char[10240];
	// strcpy(pszResp, "{\"lane_count\":2,\"async_scan\":true,\"dispenser_slot_count\":35,"delivery_type\":34,\"plc_ip\":\"192.168.1.1\",\"plc_port\":9000,\"outlet_id\":1}");

	// printf("GetConfigFromLocalCloud:: Parsing [%s]\n", pszResp);

	// Try loading into json_t
	json_t *pRoot;
	json_error_t Err;
	pRoot = json_loads(pszResp, 0, &Err);

	// Did we not get a json ptr?
	if (!pRoot)
	{
		// Dump error
		char szErr[1024] = {0};
		sprintf(szErr, "ProcessCfgResponse:: JSON error line %d: [%s]\n", Err.line, Err.text);
		DoLog(szErr, 1);

		// Bail
		return;
	}

	// Done with buffer
	delete []pszResp;

	// We need to get multiple data fields:
	// LaneCount AsyncScan SlotCount PLCIP ItemDispenseTimeout
	json_t *pLaneCount, *pAsyncScan, *pSlotCount, *pPLCIP, *pDispenseTimeout;
	json_t *pPLCType;

	/// First get the integers
	pLaneCount = json_object_get(pRoot, "lane_count");
	if (!json_is_integer(pLaneCount))
	{
		printf("ProcessCfgResponse:: Error parsing JSON @ lane_count\n");

		// de-reference
		json_decref(pRoot);

		// Bail
		return;
	}

	pSlotCount = json_object_get(pRoot, "dispenser_slot_count");
	if (!json_is_integer(pSlotCount))
	{
		DoLog("ProcessCfgResponse:: Error parsing JSON @ dispenser_slot_count\n", 1);

		// de-reference
		json_decref(pRoot);

		// Bail
		return;
	}

	pDispenseTimeout = json_object_get(pRoot, "item_dispense_timeout_secs");
	if (!json_is_integer(pDispenseTimeout))
	{
		DoLog("ProcessCfgResponse:: Error parsing JSON @ item_dispense_timeout_secs\n", 1);

		// de-reference
		json_decref(pRoot);

		// Bail
		return;
	}

	pPLCType = json_object_get(pRoot, "plc_type");
	if (!json_is_integer(pPLCType))
	{
		DoLog("ProcessCfgResponse:: Error parsing JSON @ plc_type\n", 1);

		// de-reference
		json_decref(pRoot);

		// Bail
		return;
	}

	pAsyncScan = json_object_get(pRoot, "async_scan");
	if (!json_is_boolean(pAsyncScan))
	{
		DoLog("ProcessCfgResponse:: Error parsing JSON @ async_scan\n", 1);

		// de-reference
		json_decref(pRoot);

		// Bail
		return;
	}

	/// strings
	pPLCIP = json_object_get(pRoot, "plc_ip");
	if (!json_is_string(pPLCIP))
	{
		DoLog("ProcessCfgResponse:: Error parsing JSON @ plc_ip\n", 1);

		// de-reference
		json_decref(pRoot);

		// Bail
		return;
	}

	/// Copy data over to passed struct
	memset(pCfgInfo, 0, sizeof(ConfigInfo));

	// String
	strcpy(pCfgInfo->szPLCIP, json_string_value(pPLCIP));

	// Bools
	pCfgInfo->bAsyncScan = json_boolean_value(pAsyncScan);

	// integers
	pCfgInfo->iLaneCount = json_integer_value(pLaneCount);
	pCfgInfo->iSlotCount = json_integer_value(pSlotCount);
	pCfgInfo->iDispenseTimeout = json_integer_value(pDispenseTimeout);
	pCfgInfo->iPLCType = json_integer_value(pPLCType);

	// Done, de-reference
	json_decref(pRoot);
} // void func, no return value

/// Log functions
/// Func1: General Logging. File is opened in write mode (overwritten on each run)
// Timestamped Log messages
// Params: string Log Msg [char * i.e. read-only]
void DoLog(const char *pszLogMsg, int iPriority)
{
	// Low priority log item?
	if (iPriority > g_iLogPriority)
		return;

	// Enter log lock
	pthread_mutex_lock(&g_logLock);

	// Do we have a log file PTR?
	if (!g_pLogFile)
	{
		/// No - Open the log file for writing
		/// Construct filename
		char szLogFileName[1024] = {0};
		char szTime[1024] = {0};
		time_t t;
        struct tm *tmp;

        t = time(NULL);
        tmp = localtime(&t);

        // Did we get a time?
        if (tmp)
        {
        	// Convert to string
        	strftime(szTime, sizeof(szTime), "%Y.%m.%d-%H.%M.%S", tmp);
        }
        else
        	// No timestamp available
        	strcpy(szTime, "UnknownTime");

		// Write Log file path to filename - it will create in current path
		sprintf(szLogFileName, "/opt/foodbox_plc/log/plc-log.%s.txt", szTime);

		// Open the file
		g_pLogFile = fopen(szLogFileName, "wt");
	}

	// Increment log-line counter and check if we have hit 100K log lines?
	if (++g_iLogLineCounter >= 100000)
	{
		// Go back to start of file
		fseek(g_pLogFile, 0, SEEK_SET);

		// Reset counter
		g_iLogLineCounter = 0;
	} // end of log-too-big check

	/// Construct timestamped message
	char szFullMsg[4096] = {0};
	char szTimeStamp[64] = {0};

	/// Get timestamp
	struct tm *pTimeNow;
	time_t ttTimeNow;

	// Get time_t value
	time(&ttTimeNow);

	// Convert to local time
	pTimeNow = localtime(&ttTimeNow);
	strftime(szTimeStamp, sizeof(szTimeStamp), "[%Y-%m-%d %H:%M:%S]", pTimeNow);

	// Construct message
	sprintf(szFullMsg, "%s %s", szTimeStamp, pszLogMsg);

	// Write message with CRLF [windows newline]
	fprintf(g_pLogFile, "%s\n", szFullMsg);

	// Flush buffer to ensure the data is written immediately
	fflush(g_pLogFile);

	// DEBUG DEBUG DEBUG
	printf("%s\r\n", szFullMsg);  // Only for testing purposes
	fflush(stdout);

	// Exit log lock
	pthread_mutex_unlock(&g_logLock);
} // void func, no return value


/// Linked List = Head -> node -> ... -> Tail
/// Head can be same as Tail
/// Head / Tail can both be empty
/// Tail -> Next = NULL always

// Inserts payload into LinkedList as new node
// The LinkedList is sorted by DispenseID ascending
// Params: string DispenseID [const char makes it read-only], integer Status
// Returns: Pointer to inserted node
pNode InsertListNode(char *pszDispenseID, int iStatus, char *pszOrderStub)
{
	// First create node
	pNode pNew = new Node;

	// Initialize it to NULL
	pNew->pNext = NULL;
	memset(&(pNew->PayLoad), 0, sizeof(ItemStatusNode));

	// Add payload - safe string copy of upto 10 bytes
	strncpy(pNew->PayLoad.szDispenseID, pszDispenseID, 10 * sizeof(char));
	// Add order stub - safe string copy of upto 49 bytes
	strncpy(pNew->PayLoad.szOrderStub, pszOrderStub, 59 * sizeof(char));
	pNew->PayLoad.iDispenseStage = iStatus;
	// Get time_t value (# of seconds since EPOCH)
	time(&pNew->PayLoad.ttStartTime);


	// Is head NULL? i.e. List empty?
	if (g_pHead == NULL)
	{
		// Yes - just set head + tail to this new node
		g_pHead = pNew;
	}
	else
	{
		// We need to iterate through the list till no nodes left
		// ...looking to find the right place to place this node
		// ...based on DispenseID (needs ascending sorted list position)
		// Start at the head
		pNode pIter = g_pHead;
		pNode pPrev;

		// Until we cross the tail (whose pNext is NULL)
		while (pIter)
		{
			// Is the iterator's DispenseID more than the DispenseID of the new node?
			if (strcmp(pIter->PayLoad.szDispenseID, pNew->PayLoad.szDispenseID) > 0)
				// Yes, we need to stop here
				break;

			// Iterate forward [storing previous node for insert]
			pPrev = pIter;
			pIter = pIter->pNext;
		}

		/// Edge case = pIter is the head!
		// Is this the head?
		if (pIter == g_pHead)
		{
			// Yes, we now have a new head - replace the old head with pNew
			// The old head is now the 2nd node i.e pNew->pNext
			pNew->pNext = g_pHead;
			g_pHead = pNew;
		}
		// Not the head, did we cross end of list?
		else if(!pIter)
		{
			// Change the tail - first make the old tail node point to pNew
			pPrev->pNext = pNew;
		} // end else-case: at end of list
		// Inserting after head and before tail - just do the insert
		else
		{
			// Modify the prev node of iter to point to new node
			pPrev->pNext = pNew;

			// Modify the pNew to point to pIter
			pNew->pNext = pIter;
		}	// end else-case [list non-empty case]
	}

	// Increment list size
	g_iStatusListNodeCount++;

	// Done, return the new node
	return pNew;
} // end of insert list node func


// Returns a new string holding substring of a passed string
// with passed # of characters and starting index
// Parameters:
// Returns: new string char * to be freed using delete[]
char *substr(char *pszString, int iStartIdx, int iNumChars)
{
	// Allocate string to return
	char *pszRet = new char[iNumChars + 1];

	// Initialize string
	memset(pszRet, 0, (iNumChars + 1) * sizeof(char));

	// Copy over data
	strncpy(pszRet, &pszString[iStartIdx], iNumChars);

	// Return result
	return pszRet;
}
