	//// Header file for PLC Variables
    //// The PLC maintains these variables in its internal state table and allows
    //// ..us to read them
    //// The assumption here is that these variables do not change over time
    //// ..new variables may of course be added to accommodate extensions to the functionality

    /// Some constant name prefixes:
    // g - global
    // d1, d2 - dispenser 1/2
    // del - delivery stages

    /// Some constant name abbreviations
    // BCON = Barcode + Order Number (AutoID)

    /// Some PLC variable name explanations
    // L1, C1 = Length 1, Column 1 [whatever that means]


    //// Basic variables
    // Power is turned ON [0/1]
    #define gboolPLCPowerON "Power_On"
    #define gMLboolPLCPowerON "I:0/0" // MicroLogix

    // Machine ready for operation (all drives are working) [0/1]
    #define gboolPLCAlwaysON "Always_On"
    #define gMLboolPLCAlwaysON "B3:0/0" // MicroLogix

	/// Dispenser  - SCANSTATUS
	/// - when DoorClosed = 0 + OKToOpenDoor = 1, then scan has begun
	// 0/1 - Is the door closed?
    #define d1boolDoorClosed "Disp_Main_Door_Closed"
    #define d1MLboolDoorClosed "B3:9/1"  // MicroLogix

    // 0/1 - Are we allowed to open the door?
    #define d1boolOKToOpenDoor "Dispenser:Disp_Ok_to_Open_Door"
    #define d1MLboolOKToOpenDoor "B3:9/2"  // MicroLogix

    //// Dispenser - Scanning
    #define d1boolScanStarted "Disp_Start_Scan_Bit_Longer"
    #define d1MLboolScanStarted "B3:13/8"  // MicroLogix

    /// ASYNC MODE SCAN ---- NO MICROLOGIX VARIABLES AVAILABLE
    // (the array is read once the scan complete bit is set)
    // Async mode :: Scan complete flag - set when scan complete
    #define d1boolasyncScanCompleted "Disp_Async_Scan_Complete_Bit_Longer"

    // Async mode :: Array of scanned barcodes
    #define d1asyncScannedBarcode "Disp_Async_Slot_Barcode"


	/// SYNC MODE SCAN
    // Sync Mode :: Data - One by one the barcodes + slot number show up here
	// -- each read removes one from the list @ PLC waiting to be read
    // ; keep reading until scan completion flag is set
	#define d1stringsyncBCSlot "Dispenser:Disp_Barcode_Data_1"
    #define d1MLstringsyncBCSlot "ST10:152"  // MicroLogix

    // Sync mode :: Scan Completion flag - set to 1 when scan is complete
    #define d1boolsyncScanCompleted "Disp_Scan_Complete_Bit_Longer"
    #define d1MLboolsyncScanCompleted "B3:2/4" // MicroLogix

    /// ORDER PROCESS
	// 1/0 Flag - is dispenser ready for ordering? Dont send orders until it is
    #define d1boolReadyForOrdering "Disp_Ready"
    #define d1MLboolReadyForOrdering "B3:8/8"  // MicroLogix

    // Order data chunk to be written to this var to initiate order
    #define d1stringPlaceOrder "Disp_New_Order"
    #define d1MLstringPlaceOrder "ST9:0.LEN"  // MicroLogix

	// Barcode+Order number of item which has been 'picked' - Stage 1
    #define d1stringBCONPickedItem "Disp_Picked.Barcode_Order_Number"
    #define d1MLstringBCONPickedItem "ST51:2"  // MicroLogix

    // Barcode+Order number of item in staging area - Stage 2
    #define d1stringBCONStagingItem "Disp_Staging.Barcode_Order_Number"
    #define d1MLstringBCONStagingItem "ST55:2" // MicroLogix

    // Barcode+Order number of item in rotary area - Stage 3
    #define d1stringBCONRotaryItem "Rotary.Barcode_Order_Number"
    #define d1MLstringBCONRotaryItem " " // MicroLogix Absent

    // Barcode+Order number of item in piercing area - Stage 4
    #define d1stringBCONPiercingItem "Lane_1_Start.Barcode_Order_Number"
    #define d1MLstringBCONPiercingItem " " // MicroLogix Absent



	//// DELIVERY STAGES (AFTER EXITING INDIVIDUAL SERVOS [DISPENSERS])
	/// Microwave Oven #1
	// Barcode + Order number of item at front of Microwave #1 Stage 5
    #define delstringBCONMic1FrontItem "Mic_1_Front.Barcode_Order_Number"
    #define delMLstringBCONMic1FrontItem "ST60:2" // MicroLogix

	// Barcode + Order number of item  in Microwave #1 Stage 6
    #define delstringBCONMic1InsideItem "Mic_1.Barcode_Order_Number"
    #define delMLstringBCONMic1InsideItem "ST52:2" // MicroLogix

	// [0/1] - is any item being heated in Microwave #1 Stage 7
    #define delboolFlagMic1HeatingItem "Mic_1_Heating_Started"
    #define delMLboolFlagMic1HeatingItem "B3:13/6" // MicroLogix


	/// Microwave Oven #2
	// Barcode + Order number of item at front of Microwave #2 Stage 5
    #define delstringBCONMic2FrontItem "Mic_2_Front.Barcode_Order_Number"
    #define delMLstringBCONMic2FrontItem "ST60:2" // MicroLogix

	// Barcode + Order number of item in Microwave #2 Stage 6
    #define delstringBCONMic2InsideItem "Mic_2.Barcode_Order_Number"
    #define delMLstringBCONMic2InsideItem "ST53:2" // MicroLogix

	// [0/1] - is any item being heated in Microwave #2 Stage 7
    #define delboolFlagMic2HeatingItem "Mic_2_Heating_Started"
    #define delMLboolFlagMic2HeatingItem "B3:13/7" // MicroLogix


    /// Microwave Oven #3
	// Barcode + Order number of item at front of Microwave #3 Stage 5
    #define delstringBCONMic3FrontItem "Mic_3_Front.Barcode_Order_Number"
    #define delMLstringBCONMic3FrontItem " " // MicroLogix Absent

	// Barcode + Order number of item in Microwave #3 Stage 6
    #define delstringBCONMic3InsideItem "Mic_3.Barcode_Order_Number"
    #define delMLstringBCONMic3InsideItem " " // MicroLogix Absent

	// [0/1] - is any item being heated in Microwave #3 Stage 7
    #define delboolFlagMic3HeatingItem "Mic_3_Heating_Started"
    #define delMLboolFlagMic3HeatingItem " " // MicroLogix Absent

    /// LANE CHANGE
    // Barcode + Order # of item at Lane Change [optional stage] Stage 8
    #define delstringBCONLane1ChangeItem "Lane_Change.Barcode_Order_Number"
    #define delMLstringBCONLane1ChangeItem "ST56:2" // MicroLogix


	/// DELIVERY LANE 1
	// Barcode + Order # of item at Lane 1 End [customer pick up point] Stage 9
	#define delstringBCONLane1EndItem "Lane_1_End.Barcode_Order_Number"
    #define delMLstringBCONLane1EndItem "ST59:2"  // MicroLogix

	/// DELIVERY LANE 2
	// Barcode + Order # of item at Lane 2 End [customer pick up point] Stage 9
    #define delstringBCONLane2EndItem "Lane_2_End.Barcode_Order_Number"
    #define delMLstringBCONLane2EndItem " "  // MicroLogix

    ////// END OF VARIABLE LIST //////
