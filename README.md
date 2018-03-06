# Foodbox-PLC
This is the repo for the PLCHandler service

## Source files
PLCFunctions.cpp contains the PLC interface functions (Connect/Disconnect/Read/Write)

PLCVariables.h contains the PLC Tags to be used to read/write data from variables on the PLC

PLCHandlerService.cpp/h contain the main logic

## PreRequisites to Compile
Download and compile jansson-2.7 or newer from `http://www.digip.org/jansson/` (./configure; make; make install)

Install libcurl4-openssl-dev using 

```
sudo apt-get install libcurl4-openssl-dev
```

libplc is compiled from plcio-4.4 source (or newer). To install it, ensure the PLC libraries file - libs.tar.gz from binaries Google Drive folder (at bottom of this readme) is extracted to
```
/usr/local/cti/lib
```

(if you have issues with compiling the code, try installing libcurl3 and libjansson4 using apt-get)

## Compile
First

```
make clean
```

then
```
make
```

Generates one binary: PLCHandler
## Steps to start the app
> Have a .plcrc file in the home dir of your repo
Eg -
```
export LD_LIBRARY_PATH=/usr/local/cti/lib
export LocalCloudServer=192.168.1.7:8000
```
> Source the file.
```
. .plcrc
```
> Start the app
```
./PLCHandler
```

## Drive folder for some binaries (plc libs + windows test binaries)
```
https://drive.google.com/folderview?id=0Bwhh3UJnz1plfmhzX2hrbzdrakRtb2hzNWF1V21oSGFaU2VmMENkNTMzbmxHa3g4TGhaeFk&usp=sharing
```

