#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <unistd.h>
#include "plc.h"

// #define BOOL bool
// #define FALSE false
/// Globals
// globals: Functions
PLC *ConnectToPLC(const char *pszIP, int iPort, bool bEnIP);

struct ReadStringStruct
{
  int iLen;
  char szData[83];
} ReadString, *pReadString;

// Main Func of program
main()
{
    int iRes, *piRes;
    // int iType;
    char szVar[1024] = {0};
    char szIP[] = "192.168.1.80";
    PLC *pPLC = NULL;
    char szData[100000] = {0};

    // Till world ends
    // while (1)
    {
        if (!szVar[0])
        {
            printf("Enter the name of the variable to write: ");
            fflush(stdin);
            scanf("%s", szVar);

            printf("Enter data ");
            fflush(stdin);
            scanf("%s", szData);
        }
        printf("Writing var [%s] data [%s] size [%d]\n", szVar, szData, sizeof(ReadString));

        // Connect to PLC if needed
        if (!pPLC)
            pPLC = ConnectToPLC(szIP, 44818, FALSE);

        // Clear buffer
        memset(&ReadString, 0, sizeof(ReadString));
        ReadString.iLen = strlen(szData);
        strncpy(ReadString.szData, szData, ReadString.iLen);


        iRes = plc_write(pPLC, 0, szVar, (void *)&ReadString, sizeof(ReadString), 0, "i1c82");

        // Report operation result
        if (iRes == -1 || iRes == 0)
          plc_print_error(pPLC, "plc_write");
        else if (iRes == 0)
          printf("plc_write -- No Data written\n");
        else if (iRes > 0)
          printf("plc_write -- %d bytes written\n", iRes);

        // End of block
        // printf("\n\n");
        // szResult[0] = '\0';
        usleep(0.5 * 1000000);
    }

}




// Connects to regular / enip server of PLC
// and returns a PLC fd
// This will retry until the connect succeeds
// params: IP of PLC, Port of PLC, BOOL enip (default FALSE if not supplied)
PLC *ConnectToPLC(const char *pszIP, int iPort, bool bENIP)
{
	PLC *pPLC = NULL;

	char szPLCString[1024];

	// Are we asked to connect to enipd?
	if (bENIP)
		// Yes - enipd for unsolicited messages
		sprintf(szPLCString, "enip %s", pszIP);
	else
		// No - ControlLogix PLC regular server
		sprintf(szPLCString, "cip %s", pszIP);


	// Loop will break from inside (like egg)
	while (TRUE)
	{
		// Connect
		pPLC = plc_open(szPLCString);

		// Success?
		if (pPLC)
			// Done
			break;

    // Log error to STDOUT
    plc_print_error(pPLC, "plc_open");

		// wait 10 seconds
		sleep(10);
	} // end of eternal while loop that tries to connect to PLC

	// Return PLC fd
	return pPLC;
}
