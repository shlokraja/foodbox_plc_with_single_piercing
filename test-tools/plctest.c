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
PLC *ConnectToPLC(const char *pszIP, int iPort);

struct
{
  int iLen;
  char szData[83];
} ReadString, *pReadString;

// Main Func of program
main()
{
    int iRes, *piRes;
    int iType;
    char szVar[1024] = {0};
    char szIP[] = "192.168.1.80";
    PLC *pPLC = NULL;
    char szResult[100000] = {0};

    // Till world ends
    while (1)
    {
        if (!szVar[0])
        {
            printf("Enter the name of the variable to scan: ");
            fflush(stdin);
            scanf("%s", szVar);

            printf("Enter format: 1 - BOOL, 2 - STRING82, 3 - TIMER: ");
            fflush(stdin);
            scanf("%d", &iType);
        }
        // Connect to PLC if needed
        if (!pPLC)
            pPLC = ConnectToPLC(szIP, 44818);

        // Clear buffer
        memset(szResult, 0, 1024 * sizeof(char));
        memset(&ReadString, 0, sizeof(ReadString));


        // Read Var
        switch(iType)
        {
          case 1:
            iRes = plc_read(pPLC, 0, szVar, (void *)szResult, sizeof(char), 0, "c1");
            break;
          case 2:
            iRes = plc_read(pPLC, 0, szVar, (void *)&ReadString, sizeof(ReadString), 0, "i1c82");
            if (iRes > 0)
              strcpy(szResult, ReadString.szData);
            break;
          case 3:
            iRes = plc_read(pPLC, 0, szVar, (void *)szResult, sizeof(int), 0, "i1");
            break;
          default:
            printf("Error -- Invalid Type: %d\n", iType);
            continue;
        }

        // Report operation result
        if (iRes == -1)
          plc_print_error(pPLC, "plc_read");
        else if (iRes == 0)
          printf("plc_read -- No Data received\n");
        else if (iRes > 0)
          printf("plc_read -- %d bytes read\n", iRes);

        // Print Result
        if (iType == 1)
          printf("Result: [%d]\n", szResult[0]);
        else if (iType == 3)
        {
          szResult[2] = '\0';
          piRes = (int *)szResult;
          printf("Result: [%x %x][%d]\n", szResult[0], szResult[1], *piRes);
        }
        else
          printf("Result: [%s]\n", szResult);

        // End of block
        // printf("\n\n");
        szResult[0] = '\0';
        usleep(0.1 * 1000000);
    }

}




// Connects to regular / enip server of PLC
// and returns a PLC fd
// This will retry until the connect succeeds
// params: IP of PLC, Port of PLC, BOOL enip (default FALSE if not supplied)
PLC *ConnectToPLC(const char *pszIP, int iPort)
{
	PLC *pPLC = NULL;

	char szPLCString[1024];

	// No - ControlLogix PLC regular server
	sprintf(szPLCString, "*cip %s", pszIP);


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

  plc_log_init("PLCStressLog.txt");

	// Return PLC fd
	return pPLC;
}
