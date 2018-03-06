#include "PLCHandlerService.h"

// Global functions
void DisconnectFromPLC(PLC *pPLC);
PLC *ConnectToPLC(char *pszIP, int iPort, BOOL bMicroLogix);
void *WriteVarToPLC(PLC *pPLC, char *pszVarName, char *pszVal, int iLen);
char *ReadVarFromPLC(PLC *pPLC, char *pszVarName, char cVarType);

// Global variables
struct PLCStringStruct
{
  int iLen;
  char szData[83];
} PLCString, *pPLCString;

// External vars + funcs
extern PLC *g_pOrderPLC, *g_pScanPLC;
extern ConfigInfo g_CfgInfo;
extern pthread_mutex_t g_plcLock;

extern void DoLog(const char *pszLogMsg, int iPriority = 0);


// Connects to PLC
// and returns a PLC fd
// This will retry until the connect succeeds
// params: IP of PLC, Port of PLC
PLC *ConnectToPLC(char *pszIP, int iPort, BOOL bMicroLogix)
{
  //// IMPORTANT NOTE
  /// THIS FUNCTION DOES NOT LOCK PLCIO library
  /// AS IT MAY BE INVOKED FROM WITHIN THE PLC LOCK BY PLCRead/PLCWrite
  /// (and if not, it is only invoked at PLCHandler start)

	PLC *pPLC = NULL;

	char szPLCString[1024];

  // Build PLC Connection String
	if (!bMicroLogix)
  {
      // CIP Protocol being used ControlLogix PLC
  	  snprintf(szPLCString, 1024, "cip %s", pszIP);
  } // end non-micrologix check
  else
  {
    // ABETH Protocol being used MicroLogix PLC
    snprintf(szPLCString, 1024, "cipmlx %s", pszIP);
  }

	// Loop will break from inside (like egg)
	while (TRUE)
	{
    DoLog("Connecting to PLC", 4);

		// Connect
		pPLC = plc_open(szPLCString);

		// Success?
		if (pPLC)
			// Done
			break;

    // Log error to STDOUT
    plc_print_error(pPLC, "plc_open");

    // Log error to file
    char szErr[1024] = {0};
    sprintf(szErr, "plc_open: Error [%s]", plc_open_ptr->ac_errmsg);
    DoLog(szErr);

		// wait 10 seconds
		sleep(10);
	} // end of eternal while loop that tries to connect to PLC

  DoLog("Connected to PLC", 4);

  // Wait a bit - for PLC to 'cool down'
  sleep(2); // 2 seconds

  // Debug Debug Debug
  // Set log file name (for super-enhanced PLCIO packet logging)
  // plc_log_init("PLCLogFile2.txt");

	// Return PLC fd
	return pPLC;
}

// Disconnects an fd from a PLC
void DisconnectFromPLC(PLC *pPLC)
{
  // Lock the PLCIO code
  pthread_mutex_lock(&g_plcLock);

	// Call the PLCIO library
	plc_close(pPLC);

  // Done with PLCIO - unlock
  pthread_mutex_unlock(&g_plcLock);
} // void function, no return value

// Reads variable from PLC and returns a string with the data
// Variables can be of three kinds: BOOL, String82, and int
// Parameters: Pointer to PLC, Name of variable to read, Type of var ('b'/'s'/'i')
// Returns: freshly allocated char * array
char *ReadVarFromPLC(PLC *pPLC, char *pszVarName, char cVarType)
{
    char *pszRet;
		int iTimeouts = 0;
		char szTempRet[MAXPLCREAD] = {0};

    // Lock the PLCIO code
    pthread_mutex_lock(&g_plcLock);

		// Check if bool, string, int ?
		// ..and generate format string for PLC Read function
		char szType[10] = {0};
		int iReadLen;
    int iOp;
		switch(cVarType)
    {
      case 'b':
  			strcpy(szType, "i1");
        iOp = PLC_RCOIL;
        if (g_CfgInfo.iPLCType == 0)
           iReadLen = 1;
        else
  			   iReadLen = 2;
		    break;
      case 's':
        if (g_CfgInfo.iPLCType == 0)
  			{
            strcpy(szType, "i1c82");
      			iReadLen = sizeof(PLCString);
        }
        else
        {
            strcpy(szType, "c82");
            iOp = PLC_RBYTE;
            iReadLen = 82;
        }
        break;
      default:
  			strcpy(szType, "i1");
        iOp = PLC_RREG;
  			iReadLen = 2;
		}

    // Read data from PLC
    int iBytesRead;

reader:

    // ControlLogix PLC?
    if (g_CfgInfo.iPLCType == 0)
    {
        // Regular ControlLogix PLC
        iBytesRead = plc_read(pPLC, 0, pszVarName, szTempRet, iReadLen, PLCTIMEOUT, szType);
    }
    // MicroLogix PLC!
    // Is this not a string read?
    else if (iOp != PLC_RBYTE)
        // WORD conversion
        iBytesRead = plc_read(pPLC, iOp, pszVarName, szTempRet, iReadLen, PLCTIMEOUT, PLC_CVT_WORD);
    else
        // No conversion required - string read
        iBytesRead = plc_read(pPLC, iOp, pszVarName, szTempRet, iReadLen, PLCTIMEOUT, PLC_CVT_NONE);

    // Error?
    if (iBytesRead == -1)
		{
				// Ignore invalid tag errors, some tags dont exist
				// ..and we've done enough testing to ensure we know which ones dont exist
				if (pPLC->j_error == PLCE_BAD_ADDRESS)
        {
            // Test Test Only for Scan PLC Logging
            if (pPLC == g_pScanPLC)
              printf("PLCRead:: Tag Absent: [%s]\n", pszVarName);

            // Done with PLCIO, unlock
            pthread_mutex_unlock(&g_plcLock);

            // Return No data
					  return NULL;
        }

				// Log error to STDOUT
        plc_print_error(pPLC, "plc_read");

        // Log error to file
        char szErr[1024] = {0};
        sprintf(szErr, "plc_read: Tag [%s] Type: %c Len: %d Error [%s]", \
          pszVarName, cVarType, iReadLen, pPLC->ac_errmsg);
        DoLog(szErr, 1);

				// Handle the error
				if (pPLC->j_error == PLCE_COMM_SEND || pPLC->j_error == PLCE_COMM_RECV)
				{
					/// Communication error, need to reconnect with PLC and retry
reconnect:
					// Reset timeouts
					iTimeouts = 0;

					// Was this the order plc?
					if (pPLC == g_pOrderPLC)
					{
						DoLog("PLCRead:: OrderPLC disconnected, reconnecting...");

            // Close the PLC connection
            plc_close(pPLC);

						// Reconnect order PLC
						pPLC = ConnectToPLC(g_CfgInfo.szPLCIP, g_CfgInfo.iPLCPort, g_CfgInfo.iPLCType == 1);

            DoLog("PLCRead:: OrderPLC reconnected");

						// Store in global
						g_pOrderPLC = pPLC;
					} // end of order plc check
					// No, Scan PLC?
					else if(pPLC == g_pScanPLC)
					{
						DoLog("PLCRead:: ScanPLC disconnected, reconnecting...");

            // Close the PLC connection
            plc_close(pPLC);

						// Reconnect scan plc
						pPLC = ConnectToPLC(g_CfgInfo.szPLCIP, g_CfgInfo.iPLCPort, g_CfgInfo.iPLCType == 1);

            DoLog("PLCRead:: ScanPLC reconnected");

						// Store in global
					  g_pScanPLC = pPLC;
					} // end of scan plc check
          else
          {
            // Error!
            sprintf(szErr, "PLCRead:: Invalid PLC pointer! pPLC [%d] ScanPLC [%d] OrderPLC [%d]", pPLC, g_pScanPLC, g_pOrderPLC);
            DoLog(szErr, 2);


            // Done with PLCIO, unlock
            pthread_mutex_unlock(&g_plcLock);

            // Fail
            return NULL;
          }
          
          // Retry read
          goto reader;
				} // end of if-comm send/comm recv failure check
				// No - Timeout?
				else if (pPLC->j_error == PLCE_TIMEOUT)
				{
					// Increment timeout counter
					iTimeouts++;

          /// After multiple timeouts, reconnect with PLC and retry
					if (iTimeouts == 5)
          {
              DoLog("PLCRead:: Multiple timeouts. Assuming connection failure!", 1);

							// Proceed to reconnect
							goto reconnect; // Are goto statements irredeemably evil? ;)
          }

          // Just a timeout - retry the read
          goto reader;
        } // end of timeout check

        // Nothing to do here, this error cant be handled
		} // End of bytesread = -1 check
		// Yes, we got a result
    else
    {
        // Allocate memory for return value
        char *pszRet = new char[MAXPLCREAD];

        // Clear the buffer
        memset(pszRet, 0, MAXPLCREAD);

				int *piRes;
				// What kind of value was this?
				switch(cVarType)
        {
          case 'b':
  					/// Bool: Print the 1/0 into a string
  					sprintf(pszRet, "%d", szTempRet[0]);
            break;
          case 's':
				    /// String
            if(g_CfgInfo.iPLCType == 0)
    				{
      					// Get the struct
      					pPLCString = (struct PLCStringStruct *)szTempRet;

      					// Extract the data payload
      					strcpy(pszRet, pPLCString->szData);
            }
            else
                strcpy(pszRet, szTempRet);
            break;
          default:
            /// Integer
  					// Get int *
  					piRes = (int *)szTempRet;

  					// Write it into return variable as string
  					sprintf(pszRet, "%d", szTempRet[0]);
				} // end of 'int' case block

        // Done with PLCIO - unlock
        pthread_mutex_unlock(&g_plcLock);

      	// Return the prepared value
      	return pszRet;
    } // End of successful result handling block


    // Done with PLCIO, unlock
    pthread_mutex_unlock(&g_plcLock);

    // Nothing is read
    return NULL;
} // end of PLC Read func

// Writes data to PLC var
// Parameters: PLC Pointer, Variable Name, Value to write, length in bytes, format string
void *WriteVarToPLC(PLC *pPLC, char *pszVarName, char *pszVal, int iLen)
{
	int iTimeouts = 0;
	/// Writes are ONLY String82 for now
	// Prepare Struct
	memset(&PLCString, 0, sizeof(PLCString));
	PLCString.iLen = strlen(pszVal);
	strncpy(PLCString.szData, pszVal, PLCString.iLen);

  // Lock the PLCIO code
  pthread_mutex_lock(&g_plcLock);
writer:
	/// Write to PLC
  int iBytesWritten;
  // Is this a controllogix plc?
  if (g_CfgInfo.iPLCType == 0)
      iBytesWritten  = plc_write(pPLC, 0, pszVarName, (void *)&PLCString, sizeof(PLCString), PLCTIMEOUT, "i1c82");
  else
  {
  		char szVal[100] = {0};
  		sprintf(szVal, "xx%s ", pszVal);
  		szVal[0] = 59;
  		szVal[1] = 0;
  		for (int j = 2; j <= 60; j+= 2)
  		{
  			char cTmp;
  			cTmp = szVal[j];
  			szVal[j] = szVal[j + 1];
  			szVal[j + 1] = cTmp;
  		}
      iBytesWritten  = plc_write(pPLC, PLC_WBYTE, pszVarName, (void *)szVal, 52, PLCTIMEOUT, PLC_CVT_WORD);
  }

  char szMsg[1024] = {0};
	sprintf(szMsg, "WriteVarToPLC:: Wrote: Var [%s] Data [%s] result [%d]\n", pszVarName, PLCString.szData, iBytesWritten);
	DoLog(szMsg, 5);

	// Error?
	if (iBytesWritten == -1)
	{
		// Need to check error reason
		plc_print_error(pPLC, "plc_write");

    // Log error to file
    char szErr[1024] = {0};
    sprintf(szErr, "plc_write: Error [%s][%d]", pPLC->ac_errmsg, pPLC->j_error);
    DoLog(szErr);


		// Was this a transport error?
		if (pPLC->j_error == PLCE_COMM_SEND || pPLC->j_error == PLCE_COMM_RECV)
		{
			/// Communication error, need to reconnect with PLC and retry
reconnectw:
			// Reset timeout counter
			iTimeouts = 0;

			// Was this the order PLC?
			if (pPLC == g_pOrderPLC)
			{
				DoLog("PLCRead:: OrderPLC disconnected, reconnecting...");

        // Close the PLC connection
        plc_close(pPLC);

				// Reconnect order PLC
				pPLC = ConnectToPLC(g_CfgInfo.szPLCIP, g_CfgInfo.iPLCPort, g_CfgInfo.iPLCType == 1);

        DoLog("PLCRead:: OrderPLC reconnected");

				// Store
				g_pOrderPLC = pPLC;
			} // End of orderplc check
			// No, Scan PLC?
			else if(pPLC == g_pScanPLC)
			{
				DoLog("PLCRead:: ScanPLC disconnected, reconnecting...");

        // Close the PLC connection
        plc_close(pPLC);

				// Reconnect scan plc
				pPLC = ConnectToPLC(g_CfgInfo.szPLCIP, g_CfgInfo.iPLCPort, g_CfgInfo.iPLCType == 1);

        DoLog("PLCRead:: ScanPLC reconnected");

				// Store
				g_pScanPLC = pPLC;
			} // end of scanplc check
      else // No PLC match
      {
        // Error!
        sprintf(szErr, "PLCRead:: Invalid PLC pointer! pPLC [%d] ScanPLC [%d] OrderPLC [%d]", pPLC, g_pScanPLC, g_pOrderPLC);
        DoLog(szErr, 2);

 
        // Done with PLCIO, unlock
        pthread_mutex_unlock(&g_plcLock);

        // Fail
        return NULL;
      }

      // Redo the write now that we've reconnected
      goto writer;

		} // end of check for catastrophic error
		// Just a timeout?
		else if (pPLC->j_error == PLCE_TIMEOUT)
		{
			/// Retry, and after multiple timeouts, reconnect with PLC and retry
			// Increment timeout counter
			iTimeouts++;

			// More than 5?
			if (iTimeouts == 5)
					// Proceed to reconnect
					goto reconnectw; // Are goto statements evil? ;)
		} // end of timeout check

    // Retry write
    goto writer;
	} // end of error check

  // UnLock the PLCIO code
  pthread_mutex_unlock(&g_plcLock);
} // end of PLC write func, no return value
