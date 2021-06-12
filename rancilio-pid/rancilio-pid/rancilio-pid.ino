/********************************************************
   Version 2.9.2 (29.04.2021)  
******************************************************/

/********************************************************
  INCLUDES
******************************************************/
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include "userConfig.h" // needs to be configured by the user
#include <U8g2lib.h>
#include "PID_v1.h" //for PID calculation
#include "languages.h" // for language translation
#include <DallasTemperature.h>    //Library for dallas temp sensor
#if defined(ESP8266) 
  #include <BlynkSimpleEsp8266.h>
#endif
#if defined(ESP32) 
  #include <BlynkSimpleEsp32.h>
  #include <os.h> 
  hw_timer_t * timer = NULL;
#endif
#include "icon.h"   //user icons for display
#include <ZACwire.h> //NEW TSIC LIB
#include <PubSubClient.h>
#include "TSIC.h"       //Library for TSIC temp sensor
#include <Adafruit_VL53L0X.h> //for TOF 

#if (BREWMODE == 2 || ONLYPIDSCALE == 1)
#include <HX711_ADC.h>
#endif

/********************************************************
  DEFINES
******************************************************/
MACHINE machine = (enum MACHINE) MACHINEID;

#define DEBUGMODE   // Debug mode is active if #define DEBUGMODE is set

//#define BLYNK_PRINT Serial    // In detail debugging for blynk
//#define BLYNK_DEBUG

#ifndef DEBUGMODE
#define DEBUG_println(a)
#define DEBUG_print(a)
#define DEBUGSTART(a)
#else
#define DEBUG_println(a) Serial.println(a);
#define DEBUG_print(a) Serial.print(a);
#define DEBUGSTART(a) Serial.begin(a);
#endif
#define HIGH_ACCURACY

#include "DebugStreamManager.h"
DebugStreamManager debugStream;

#include "PeriodicTrigger.h" // Trigger, der alle x Millisekunden auf true schaltet
PeriodicTrigger writeDebugTrigger(5000); // trigger alle 5000 ms
PeriodicTrigger logbrew(500);


/********************************************************
  definitions below must be changed in the userConfig.h file
******************************************************/
int Offlinemodus = OFFLINEMODUS;
const int OnlyPID = ONLYPID;
const int TempSensor = TEMPSENSOR;
const int Brewdetection = BREWDETECTION;
const int fallback = FALLBACK;
const int triggerType = TRIGGERTYPE;
const int VoltageSensorType = VOLTAGESENSORTYPE;
const boolean ota = OTA;
const int grafana = GRAFANA;
const unsigned long wifiConnectionDelay = WIFICINNECTIONDELAY;
const unsigned int maxWifiReconnects = MAXWIFIRECONNECTS;
//int machineLogo = MACHINELOGO;
const unsigned long brewswitchDelay = BREWSWITCHDELAY;
int BrewMode = BREWMODE ;
int machinestate = 0;
int lastmachinestate = 0;


//Display
uint8_t oled_i2c = OLED_I2C;

//TOF
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
int calibration_mode = CALIBRATION_MODE;
uint8_t tof_i2c = TOF_I2C;
int water_full = WATER_FULL;
int water_empty = WATER_EMPTY;
unsigned long previousMillisTOF;  // initialisation at the end of init()
const unsigned long intervalTOF = 5000 ; //ms
double distance;
double percentage;

// Wifi
const char* hostname = HOSTNAME;
const char* auth = AUTH;
const char* ssid = D_SSID;
const char* pass = PASS;
unsigned long lastWifiConnectionAttempt = millis();
unsigned int wifiReconnects = 0; //actual number of reconnects

// OTA
const char* OTAhost = OTAHOST;
const char* OTApass = OTAPASS;

//Blynk
const char* blynkaddress  = BLYNKADDRESS;
const int blynkport = BLYNKPORT;
unsigned int blynkReCnctFlag;  // Blynk Reconnection Flag
unsigned int blynkReCnctCount = 0;  // Blynk Reconnection counter
unsigned long lastBlynkConnectionAttempt = millis();

//backflush values
const unsigned long fillTime = FILLTIME;
const unsigned long flushTime = FLUSHTIME;
int maxflushCycles = MAXFLUSHCYCLES;

//MQTT
WiFiClient net;
PubSubClient mqtt(net);
const char* mqtt_server_ip = MQTT_SERVER_IP;
const int mqtt_server_port = MQTT_SERVER_PORT;
const char* mqtt_username = MQTT_USERNAME;
const char* mqtt_password = MQTT_PASSWORD;
const char* mqtt_topic_prefix = MQTT_TOPIC_PREFIX;
char topic_will[256];
char topic_set[256];
unsigned long lastMQTTConnectionAttempt = millis();
unsigned int MQTTReCnctFlag;  // Blynk Reconnection Flag
unsigned int MQTTReCnctCount = 0;  // Blynk Reconnection counter

//Voltage Sensor
unsigned long previousMillisVoltagesensorreading = millis();
const unsigned long intervalVoltagesensor= 200 ;
int VoltageSensorON, VoltageSensorOFF;

// QuickMill thermoblock steam-mode (only for BREWDETECTION = 3)
const int maxBrewDurationForSteamModeQM_ON =  200; // if brewtime is shorter steam-mode starts
const int minPVSOffTimedForSteamModeQM_OFF = 1500; // if PVS-off-time is longer steam-mode ends
unsigned long timePVStoON = 0;                     // time pinvoltagesensor switched to ON
unsigned long lastTimePVSwasON = 0;                // last time pinvoltagesensor was ON
bool steamQM_active = false;                       // steam-mode is active
bool brewSteamDetectedQM = false;                  // brew/steam detected, not sure yet what it is
bool coolingFlushDetectedQM = false;

//Pressure sensor
#if (PRESSURESENSOR == 1) // Pressure sensor connected
int offset = OFFSET;
int fullScale = FULLSCALE;
int maxPressure = MAXPRESSURE;
float inputPressure = 0;
const unsigned long intervalPressure = 200;
unsigned long previousMillisPressure;  // initialisation at the end of init()
#endif


/********************************************************
   declarations
******************************************************/
int pidON = 1 ;                 // 1 = control loop in closed loop
int relayON, relayOFF;          // used for relay trigger type. Do not change!
boolean kaltstart = true;       // true = Rancilio started for first time
boolean emergencyStop = false;  // Notstop bei zu hoher Temperatur
double EmergencyStopTemp = 120; // Temp EmergencyStopTemp
const char* sysVersion PROGMEM  = "Version 2.9.2 MASTER";   //System version
int inX = 0, inY = 0, inOld = 0, inSum = 0; //used for filter()
int bars = 0; //used for getSignalStrength()
boolean brewDetected = 0;
boolean setupDone = false;
int backflushON = 0;            // 1 = activate backflush
int flushCycles = 0;            // number of active flush cycles
int backflushState = 10;        // counter for state machine

/********************************************************
   moving average - brewdetection
*****************************************************/
const int numReadings = 15;             // number of values per Array
double readingstemp[numReadings];        // the readings from Temp
unsigned long readingstime[numReadings];        // the readings from time
double readingchangerate[numReadings];

int readIndex = 1;              // the index of the current reading
double total = 0;               // total sum of readingchangerate[]
double heatrateaverage = 0;     // the average over the numReadings
double changerate = 0;          // local change rate of temprature
double heatrateaveragemin = 0 ;
unsigned long  timeBrewdetection = 0 ;
int timerBrewdetection = 0 ;    // flag is set if brew was detected
int firstreading = 1 ;          // Ini of the field, also used for sensor check

/********************************************************
   PID - values for offline brewdetection
*****************************************************/
double aggbKp = AGGBKP;
double aggbTn = AGGBTN;
double aggbTv = AGGBTV;
#if aggbTn == 0
double aggbKi = 0;
#else
double aggbKi = aggbKp / aggbTn;
#endif
double aggbKd = aggbTv * aggbKp ;
double brewtimersoftware = 45;    // 20-5 for detection
double brewboarder = 150 ;        // border for the detection, be carefull: to low: risk of wrong brew detection and rising temperature
const int PonE = PONE;

/********************************************************
   BREW INI 1 = Normale Prefinfusion , 2 = Scale & Shottimer = 2
******************************************************/

  #include "brewscaleini.h"

/********************************************************
   Sensor check
******************************************************/
boolean sensorError = false;
int error = 0;
int maxErrorCounter = 10 ;  //depends on intervaltempmes* , define max seconds for invalid data

/********************************************************
   PID
******************************************************/
unsigned long previousMillistemp;  // initialisation at the end of init()
const unsigned long intervaltempmestsic = 400 ;
const unsigned long intervaltempmesds18b20 = 400  ;
int pidMode = 1; //1 = Automatic, 0 = Manual

const unsigned int windowSize = 1000;
unsigned int isrCounter = 0;  // counter for ISR
unsigned long windowStartTime;
double Input, Output;
double setPointTemp;
double previousInput = 0;

double BrewSetPoint = SETPOINT;
double setPoint = BrewSetPoint;
double SteamSetPoint = STEAMSETPOINT;
int    SteamON = 0;
int    SteamFirstON = 0;
double aggKp = AGGKP;
double aggTn = AGGTN;
double aggTv = AGGTV;
double startKp = STARTKP;
double startTn = STARTTN;
#if startTn == 0
double startKi = 0;
#else
double startKi = startKp / startTn;
#endif

#if aggTn == 0
double aggKi = 0;
#else
double aggKi = aggKp / aggTn;
#endif
double aggKd = aggTv * aggKp ;

PID bPID(&Input, &Output, &setPoint, aggKp, aggKi, aggKd, PonE, DIRECT);    //PID initialisation

/********************************************************
   DALLAS TEMP
******************************************************/
OneWire oneWire(ONE_WIRE_BUS);         // Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
DallasTemperature sensors(&oneWire);   // Pass our oneWire reference to Dallas Temperature.
DeviceAddress sensorDeviceAddress;     // arrays to hold device address

/********************************************************
   Temp Sensors TSIC 306
******************************************************/
uint16_t temperature = 0;     // internal variable used to read temeprature
float Temperatur_C = 0;       // internal variable that holds the converted temperature in °C

#if (ONE_WIRE_BUS == 16 && TEMPSENSOR  == 2 && defined(ESP8266)) 
TSIC Sensor1(ONE_WIRE_BUS);   // only Signalpin, VCCpin unused by default
#else 
ZACwire<ONE_WIRE_BUS> Sensor2(306);    // set pin "2" to receive signal from the TSic "306"
#endif
/********************************************************
   BLYNK
******************************************************/
//Update Intervall zur App
unsigned long previousMillisBlynk;  // initialisation at the end of init()
const unsigned long intervalBlynk = 1000;
int blynksendcounter = 1;


/********************************************************
  Get Wifi signal strength and set bars for display
*****************************************************/
void getSignalStrength() {
  if (Offlinemodus == 1) return;

  long rssi;
  if (WiFi.status() == WL_CONNECTED) {
    rssi = WiFi.RSSI();
  } else {
    rssi = -100;
  }

  if (rssi >= -50) {
    bars = 4;
  } else if (rssi < -50 & rssi >= -65) {
    bars = 3;
  } else if (rssi < -65 & rssi >= -75) {
    bars = 2;
  } else if (rssi < -75 & rssi >= -80) {
    bars = 1;
  } else {
    bars = 0;
  }
}


/********************************************************
   DISPLAY Define & template
******************************************************/
//DISPLAY constructor, change if needed
#if  DISPLAY == 1
    U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);   //e.g. 1.3"
#endif
#if DISPLAY == 2
    U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);    //e.g. 0.96"
#endif
//Update für Display
unsigned long previousMillisDisplay;  // initialisation at the end of init()
const unsigned long intervalDisplay = 500;

//Standard Display or vertikal?
#if (DISPLAY == 1 || DISPLAY == 2) // Display is used 
  #if (DISPLAYTEMPLATE < 20) // normal templates
    #include "display.h"  
  #endif  
  #if (DISPLAYTEMPLATE >= 20) // vertical templates 
    #include "Displayrotateupright.h"  
  #endif  
  #if (DISPLAYTEMPLATE == 1)
      #include "Displaytemplatestandard.h"
  #endif    
  #if (DISPLAYTEMPLATE == 2)
      #include "Displaytemplateminimal.h"
  #endif    
  #if (DISPLAYTEMPLATE == 3)
      #include "Displaytemplatetemponly.h"
  #endif   
  #if (DISPLAYTEMPLATE == 4)
      #include "Displaytemplatescale.h"
  #endif   
  #if (DISPLAYTEMPLATE == 20)
      #include "Displaytemplateupright.h"
  #endif   
#endif



/********************************************************
   BLYNK define pins and read values
******************************************************/
BLYNK_CONNECTED() {
  if (Offlinemodus == 0) {
    Blynk.syncAll();
    //rtc.begin();
  }
}

BLYNK_WRITE(V4) {
  aggKp = param.asDouble();
}

BLYNK_WRITE(V5) {
  aggTn = param.asDouble();
}
BLYNK_WRITE(V6) {
  aggTv =  param.asDouble();
}

BLYNK_WRITE(V7) {
  if (machinestate == 10) //antiwindup by coldstart 
  {
    bPID.SetOutputLimits(0.0, 1.0);  // Forces minimum up to 0.0
    bPID.SetOutputLimits(-1.0, 0.0);  // Forces maximum down to 0.0
    bPID.Compute() ;
    bPID.SetOutputLimits(0, windowSize); // reset window
  }
  BrewSetPoint = param.asDouble();
  mqtt_publish("BrewSetPoint", number2string(BrewSetPoint));
}

BLYNK_WRITE(V8) {
  brewtime = param.asDouble() * 1000;
  mqtt_publish("brewtime", number2string(brewtime/1000));
}

BLYNK_WRITE(V9) {
  preinfusion = param.asDouble() * 1000;
  mqtt_publish("preinfusion", number2string(preinfusion/1000));
}

BLYNK_WRITE(V10) {
  preinfusionpause = param.asDouble() * 1000;
  mqtt_publish("preinfusionpause", number2string(preinfusionpause/1000));
}
BLYNK_WRITE(V13)
{
  pidON = param.asInt();
  mqtt_publish("pidON", number2string(pidON));
}
BLYNK_WRITE(V15) 
{
  SteamON = param.asInt();
  if (SteamON == 1) 
  {
  SteamFirstON = 1;  
  }
  if (SteamON == 0) 
  {
  SteamFirstON = 0;  
  }
  mqtt_publish("SteamON", number2string(SteamON));
}
BLYNK_WRITE(V16) {
  SteamSetPoint = param.asDouble();
  mqtt_publish("SteamSetPoint", number2string(SteamSetPoint));
}
#if (BREWMODE == 2)
BLYNK_WRITE(V18)
{
  weightSetpoint = param.asFloat();
}
#endif
BLYNK_WRITE(V25)
{
  calibration_mode = param.asInt();//
}
BLYNK_WRITE(V26)
{
  water_empty = param.asInt();//
}
BLYNK_WRITE(V27)
{
  water_full = param.asInt();//
}

BLYNK_WRITE(V30)
{
  aggbKp = param.asDouble();//
}

BLYNK_WRITE(V31) {
  aggbTn = param.asDouble();
}
BLYNK_WRITE(V32) {
  aggbTv =  param.asDouble();
}
BLYNK_WRITE(V33) {
  brewtimersoftware =  param.asDouble();
}
BLYNK_WRITE(V34) {
  brewboarder =  param.asDouble();
}
BLYNK_WRITE(V40) {
  backflushON =  param.asInt();
}

#if (COLDSTART_PID == 2)  // 2=?Blynk values, else default starttemp from config
  BLYNK_WRITE(V11) 
    {
    startKp = param.asDouble();
    }
  BLYNK_WRITE(V14)
    {
      startTn = param.asDouble();
    }
 #endif


#if (PRESSURESENSOR == 1) // Pressure sensor connected

/********************************************************
  Pressure sensor
  Verify before installation: meassured analog input value (should be 3,300 V for 3,3 V supply) and respective ADC value (3,30 V = 1023)
*****************************************************/
void checkPressure() {
  float inputPressureFilter = 0;
  unsigned long currentMillisPressure = millis();
  if (currentMillisPressure - previousMillisPressure >= intervalPressure)
  {
    previousMillisPressure = currentMillisPressure;
    
    inputPressure = ((analogRead(PINPRESSURESENSOR) - offset) * maxPressure * 0.0689476) / (fullScale - offset);    // pressure conversion and unit conversion [psi] -> [bar]
    inputPressureFilter = filter(inputPressure);
    DEBUG_print("pressure raw: ");
    DEBUG_println(inputPressure);
    DEBUG_print("pressure filtered: ");
    DEBUG_print(inputPressureFilter);
  }  
}

#endif

 
/********************************************************
  Trigger for Rancilio E Machine
******************************************************/
unsigned long previousMillisETrigger ;  // initialisation at the end of init()
const unsigned long intervalETrigger = ETRIGGERTIME ; // in Seconds
int relayETriggerON, relayETriggerOFF;    
/********************************************************
  Emergency stop inf temp is to high
*****************************************************/
void testEmergencyStop() {
  if (Input > EmergencyStopTemp && emergencyStop == false) {
    emergencyStop = true;
  } else if (Input < 100 && emergencyStop == true) {
    emergencyStop = false;
  }
}

/********************************************************
  Moving average - brewdetection (SW)
*****************************************************/
void movAvg() {
  if (firstreading == 1) {
    for (int thisReading = 0; thisReading < numReadings; thisReading++) {
      readingstemp[thisReading] = Input;
      readingstime[thisReading] = 0;
      readingchangerate[thisReading] = 0;
    }
    firstreading = 0 ;
  }

  readingstime[readIndex] = millis() ;
  readingstemp[readIndex] = Input ;

  if (readIndex == numReadings - 1) {
    changerate = (readingstemp[numReadings - 1] - readingstemp[0]) / (readingstime[numReadings - 1] - readingstime[0]) * 10000;
  } else {
    changerate = (readingstemp[readIndex] - readingstemp[readIndex + 1]) / (readingstime[readIndex] - readingstime[readIndex + 1]) * 10000;
  }

  readingchangerate[readIndex] = changerate ;
  total = 0 ;
  for (int i = 0; i < numReadings; i++)
  {
    total += readingchangerate[i];
  }

  heatrateaverage = total / numReadings * 100 ;
  if (heatrateaveragemin > heatrateaverage) {
    heatrateaveragemin = heatrateaverage ;
  }

  if (readIndex >= numReadings - 1) {
    // ...wrap around to the beginning:
    readIndex = 0;
  } else {
    readIndex++;
  }
}


/********************************************************
  check sensor value.
  If < 0 or difference between old and new >25, then increase error.
  If error is equal to maxErrorCounter, then set sensorError
*****************************************************/
boolean checkSensor(float tempInput) {
  boolean sensorOK = false;
  boolean badCondition = ( tempInput < 0 || tempInput > 150 || fabs(tempInput - previousInput) > 5);
  if ( badCondition && !sensorError) {
    error++;
    sensorOK = false;
    if (error >= 5) // warning after 5 times error
    {
     debugStream.writeW("*** WARNING: temperature sensor reading: consec_errors = %i, temp_current = %.1f",error,tempInput);
    }
  } else if (badCondition == false && sensorOK == false) {
    error = 0;
    sensorOK = true;
  }
  if (error >= maxErrorCounter && !sensorError) {
    sensorError = true ;
    debugStream.writeE("*** ERROR: temperature sensor malfunction: temp_current = %.1f",tempInput);
  } else if (error == 0 && sensorError) {
    sensorError = false ;
  }
  return sensorOK;
}

/********************************************************
  Refresh temperature.
  Each time checkSensor() is called to verify the value.
  If the value is not valid, new data is not stored.
*****************************************************/
void refreshTemp() {
  unsigned long currentMillistemp = millis();
  previousInput = Input ;
  if (TempSensor == 1)
  {
    if (currentMillistemp - previousMillistemp >= intervaltempmesds18b20)
    {
      previousMillistemp = currentMillistemp;
      sensors.requestTemperatures();
      if (!checkSensor(sensors.getTempCByIndex(0)) && firstreading == 0) return;  //if sensor data is not valid, abort function; Sensor must be read at least one time at system startup
      Input = sensors.getTempCByIndex(0);
      if (Brewdetection != 0) {
        movAvg();
      } else if (firstreading != 0) {
        firstreading = 0;
      }
    }
  }
  if (TempSensor == 2)
  {
    if (currentMillistemp - previousMillistemp >= intervaltempmestsic)
    {
      previousMillistemp = currentMillistemp;
      /*  variable "temperature" must be set to zero, before reading new data
            getTemperature only updates if data is valid, otherwise "temperature" will still hold old values
      */
      temperature = 0;
       #if (ONE_WIRE_BUS == 16 && defined(ESP8266))
         Sensor1.getTemperature(&temperature);
         Temperatur_C = Sensor1.calc_Celsius(&temperature);
         #endif
       #if ((ONE_WIRE_BUS != 16 && defined(ESP8266)) || defined(ESP32))
        Temperatur_C = Sensor2.getTemp();
        DEBUG_print(Temperatur_C);
       #endif
      //Temperatur_C = 70;
      if (!checkSensor(Temperatur_C) && firstreading == 0) return;  //if sensor data is not valid, abort function; Sensor must be read at least one time at system startup
      Input = Temperatur_C;
      if (Brewdetection != 0) {
        movAvg();
      } else if (firstreading != 0) {
        firstreading = 0;
      }
    }
  }
}

/*******************************************************
      BREWVOID.H & SCALEVOID
*****************************************************/

#include "brewvoid.h"
#include "scalevoid.h"

/*******************************************************
  Switch to offline modeif maxWifiReconnects were exceeded
  during boot
*****************************************************/
void initOfflineMode() 
{
  #if DISPLAY != 0
    displayMessage("", "", "", "", "Begin Fallback,", "No Wifi");
  #endif
  debugStream.writeI("Start offline mode with eeprom values, no wifi:(");
  Offlinemodus = 1 ;

  EEPROM.begin(1024);  // open eeprom
  double dummy; // check if eeprom values are numeric (only check first value in eeprom)
  EEPROM.get(0, dummy);
  debugStream.writeI("check eeprom 0x00 in dummy: %f",dummy);
  if (!isnan(dummy)) {
    EEPROM.get(0, aggKp);
    EEPROM.get(10, aggTn);
    EEPROM.get(20, aggTv);
    EEPROM.get(30, BrewSetPoint);
    EEPROM.get(40, brewtime);
    EEPROM.get(50, preinfusion);
    EEPROM.get(60, preinfusionpause);
    EEPROM.get(90, aggbKp);
    EEPROM.get(100, aggbTn);
    EEPROM.get(110, aggbTv);
    EEPROM.get(120, brewtimersoftware);
    EEPROM.get(130, brewboarder);
  } else {
    #if DISPLAY != 0
      displayMessage("", "", "", "", "No eeprom,", "Values");
     #endif
    debugStream.writeI("No working eeprom value, I am sorry, but use default offline value  :)");
    delay(1000);
  }
  // eeeprom schließen
  EEPROM.commit();
}

/*******************************************************
   Check if Wifi is connected, if not reconnect
   abort function if offline, or brew is running
*****************************************************/
void checkWifi() {
  if (Offlinemodus == 1 || brewcounter > 11) return;
  do {
    if ((millis() - lastWifiConnectionAttempt >= wifiConnectionDelay) && (wifiReconnects <= maxWifiReconnects)) {
      int statusTemp = WiFi.status();
      if (statusTemp != WL_CONNECTED) {   // check WiFi connection status
        lastWifiConnectionAttempt = millis();
        wifiReconnects++;
        debugStream.writeI("Attempting WIFI reconnection: %i",wifiReconnects);
        if (!setupDone) {
           #if DISPLAY != 0
            displayMessage("", "", "", "", langstring_wifirecon, String(wifiReconnects));
          #endif
        }
        WiFi.disconnect();
        WiFi.begin(ssid, pass);   // attempt to connect to Wifi network
        int count = 1;
        while (WiFi.status() != WL_CONNECTED && count <= 20) {
          delay(100);   //give WIFI some time to connect
          count++;      //reconnect counter, maximum waiting time for reconnect = 20*100ms
        }
      }
    }
    yield();  //Prevent WDT trigger
  } while ( !setupDone && wifiReconnects < maxWifiReconnects && WiFi.status() != WL_CONNECTED);   //if kaltstart ist still true when checkWifi() is called, then there was no WIFI connection at boot -> connect or offlinemode

  if (wifiReconnects >= maxWifiReconnects && !setupDone) {   // no wifi connection after boot, initiate offline mode (only directly after boot)
    initOfflineMode();
  }

}

/*******************************************************
   Check if Blynk is connected, if not reconnect
   abort function if offline, or brew is running
   blynk is also using maxWifiReconnects!
*****************************************************/
void checkBlynk() {
  if (Offlinemodus == 1 || brewcounter > 11) return;
  if ((millis() - lastBlynkConnectionAttempt >= wifiConnectionDelay) && (blynkReCnctCount <= maxWifiReconnects)) {
    int statusTemp = Blynk.connected();
    if (statusTemp != 1) {   // check Blynk connection status
      lastBlynkConnectionAttempt = millis();        // Reconnection Timer Function
      blynkReCnctCount++;  // Increment reconnection Counter
      debugStream.writeI("Attempting blynk reconnection: %i",blynkReCnctCount);
      Blynk.connect(3000);  // Try to reconnect to the server; connect() is a blocking function, watch the timeout!
    }
  }
}



/*******************************************************
   Check if MQTT is connected, if not reconnect
   abort function if offline, or brew is running
   MQTT is also using maxWifiReconnects!
*****************************************************/
void checkMQTT(){
  if (Offlinemodus == 1 || brewcounter > 11) return;
  if ((millis() - lastMQTTConnectionAttempt >= wifiConnectionDelay) && (MQTTReCnctCount <= maxWifiReconnects)) {
    int statusTemp = mqtt.connected();
    if (statusTemp != 1) {   // check Blynk connection status
      lastMQTTConnectionAttempt = millis();        // Reconnection Timer Function
      MQTTReCnctCount++;  // Increment reconnection Counter
      debugStream.writeI("Attempting MQTT reconnection: %i",MQTTReCnctCount);
      if (mqtt.connect(hostname, mqtt_username, mqtt_password,topic_will,0,0,"exit") == true);{
        mqtt.subscribe(topic_set);
        debugStream.writeI("Subscribe to MQTT Topics");
      }  // Try to reconnect to the server; connect() is a blocking function, watch the timeout!
    }
  }
}

/*******************************************************
   Convert double, float int and uint to char
   for MQTT Publish
*****************************************************/
char number2string_double[22];
char* number2string(double in) {
  snprintf(number2string_double, sizeof(number2string_double), "%0.2f", in);
  return number2string_double;
}
char number2string_float[22];
char* number2string(float in) {
  snprintf(number2string_float, sizeof(number2string_float), "%0.2f", in);
  return number2string_float;
}
char number2string_int[22];
char* number2string(int in) {
  snprintf(number2string_int, sizeof(number2string_int), "%d", in);
  return number2string_int;
}
char number2string_uint[22];
char* number2string(unsigned int in) {
  snprintf(number2string_uint, sizeof(number2string_uint), "%u", in);
  return number2string_uint;
}

/*******************************************************
   Publish Data to MQTT
*****************************************************/
bool mqtt_publish(char* reading, char* payload) {
  if (MQTT == 1){
    char topic[120];
    snprintf(topic, 120, "%s%s/%s", mqtt_topic_prefix, hostname, reading);
    mqtt.publish(topic,payload,true);
  }
  }

/********************************************************
  send data to Blynk server
*****************************************************/

void sendToBlynk() {
  if (Offlinemodus == 1) return;

  unsigned long currentMillisBlynk = millis();
  unsigned long currentMillistemp = 0;

  if (currentMillisBlynk - previousMillisBlynk >= intervalBlynk) {

    //MQTT
    if (MQTT == 1) {
      checkMQTT();
    }

    previousMillisBlynk = currentMillisBlynk;
    if (Blynk.connected()) {
      if (blynksendcounter == 1) {
        Blynk.virtualWrite(V2, Input);
        mqtt_publish("temperature", number2string(Input));
      }
      if (blynksendcounter == 2) {
        Blynk.virtualWrite(V23, Output);
      }
      if (blynksendcounter == 3) {
        Blynk.virtualWrite(V17, setPoint);
        //MQTT
        mqtt_publish("setPoint", number2string(setPoint));
      }
      if (blynksendcounter == 4) {
        Blynk.virtualWrite(V35, heatrateaverage);
      }
      if (blynksendcounter == 5) {
        Blynk.virtualWrite(V36, heatrateaveragemin);
      }
      if (grafana == 1 && blynksendcounter >= 6) {
        // Blynk.virtualWrite(V60, Input, Output, bPID.GetKp(), bPID.GetKi(), bPID.GetKd(), setPoint );
        Blynk.virtualWrite(V60, Input, Output, bPID.GetKp(), bPID.GetKi(), bPID.GetKd(), setPoint, heatrateaverage);
         if (MQTT == 1)
         {
            mqtt_publish("HeaterPower", number2string(Output));
            mqtt_publish("Kp", number2string(bPID.GetKp()));
            mqtt_publish("Ki", number2string(bPID.GetKi()));
            mqtt_publish("pidON", number2string(pidON));
            mqtt_publish("brewtime", number2string(brewtime/1000));
            mqtt_publish("preinfusionpause", number2string(preinfusionpause/1000));
            mqtt_publish("preinfusion", number2string(preinfusion/1000));
            mqtt_publish("SteamON", number2string(SteamON));
         }
        blynksendcounter = 0;
      } else if (grafana == 0 && blynksendcounter >= 5) {
        blynksendcounter = 0;
      }
      blynksendcounter++;
    }
  }
}

/********************************************************
    Brewdetection
******************************************************/
void brewdetection() 
{
  if (brewboarder == 0) return; //abort brewdetection if deactivated

  // Brew detecion == 1 software solution , == 2 hardware == 3 Voltagesensor 

  if (Brewdetection == 1) 
  {  // Bezugstimmer für SW aktivieren
     if (timerBrewdetection == 1)
    {
     bezugsZeit = millis() - timeBrewdetection ;
     }
    // Bezugstimmer für SW deaktivieren nach ende BD PID
    if (millis() - timeBrewdetection > brewtimersoftware * 1000 && timerBrewdetection == 1 )
    {
      timerBrewdetection = 0 ;    //rearm brewdetection
      if (machinestate != 30)  // Bei Onlypid = 1, bezugsZeit > 0, no reset of bezugsZeit in case of brewing. 
      {
        bezugsZeit = 0 ;
      }
     }
  } else if (Brewdetection == 2) 
  {
    if (millis() - timeBrewdetection > brewtimersoftware * 1000 && timerBrewdetection == 1 ) 
    {
      timerBrewdetection = 0 ;  //rearm brewdetection
    }
  } else if (Brewdetection == 3) 
  {
    // Bezugszeit hochzaehlen    
    if (( digitalRead(PINVOLTAGESENSOR) == VoltageSensorON) && brewDetected == 1)
       {
       bezugsZeit = millis() - startZeit ;
       lastbezugszeit = bezugsZeit ;
       }
    //  OFF: Bezug zurücksetzen
    if 
     ((digitalRead(PINVOLTAGESENSOR) == VoltageSensorOFF) && (brewDetected == 1 || coolingFlushDetectedQM == true) )
      {
        brewDetected = 0;
        timePVStoON = bezugsZeit; // for QuickMill
        bezugsZeit = 0 ; 
        startZeit = 0;
        coolingFlushDetectedQM = false;
        debugStream.writeI("HW Brew - Voltage Sensor - End");
     //   lastbezugszeitMillis = millis(); // Bezugszeit für Delay 
      }
    if (millis() - timeBrewdetection > brewtimersoftware * 1000 && timerBrewdetection == 1) // reset PID Brew
    {
      timerBrewdetection = 0 ;    //rearm brewdetection
    }
  }
  
  // Activate the BD 

  if ( Brewdetection == 1) // SW BD
  {
    if (heatrateaverage <= -brewboarder && timerBrewdetection == 0 && (fabs(Input - BrewSetPoint) < 5)) // BD PID only +/- 4 Grad Celsius, no detection if HW was active
    {
      debugStream.writeI("SW Brew detected") ;
      timeBrewdetection = millis() ;
      timerBrewdetection = 1 ;
    }
  } else if (Brewdetection == 2) // HW BD
  {
    if (brewcounter > 10 && brewDetected == 0 && brewboarder != 0) 
    {
      debugStream.writeI("HW Brew detected") ;
      timeBrewdetection = millis() ;
      timerBrewdetection = 1 ;
      brewDetected = 1;
    }  
  } else if (Brewdetection == 3) // voltage sensor 
  {
    switch (machine) {

      case QuickMill:

      if (!coolingFlushDetectedQM) 
      {
        int pvs = digitalRead(PINVOLTAGESENSOR);
        if (pvs == VoltageSensorON && brewDetected == 0 && brewSteamDetectedQM == 0 && !steamQM_active) 
        {
          timeBrewdetection = millis();
          timePVStoON = millis();
          timerBrewdetection = 1;
          brewDetected = 0;
          lastbezugszeit = 0;
          brewSteamDetectedQM = 1;
          debugStream.writeI("Quick Mill: setting brewSteamDetectedQM = 1");
          logbrew.reset();
        }

        //if (brewSteamDetectedQM == 1) 
        const int minBrewDurationForSteamModeQM_ON = 25;
        if (brewSteamDetectedQM == 1 && millis() - timePVStoON > minBrewDurationForSteamModeQM_ON)
        {
          if (pvs == VoltageSensorOFF)
          {
            brewSteamDetectedQM = 0;

            if (millis() - timePVStoON < maxBrewDurationForSteamModeQM_ON)
            {
              debugStream.writeI("Quick Mill: steam-mode detected");
              initSteamQM();
            } else {
              debugStream.writeE("*** ERROR: QuickMill: neither brew nor steam");
            }
          } 
          else if (millis() - timePVStoON > maxBrewDurationForSteamModeQM_ON)
          {
            if( Input < BrewSetPoint + 2) {
              debugStream.writeI("Quick Mill: brew-mode detected");
              startZeit = timePVStoON; 
              brewDetected = 1;
              brewSteamDetectedQM = 0;
            } else {
              debugStream.writeI("Quick Mill: cooling-flush detected");
              coolingFlushDetectedQM = true;
              brewSteamDetectedQM = 0;
            }
          }
        }
      }
      break;
      // no Quickmill: 
      default:
      previousMillisVoltagesensorreading = millis();
      if (digitalRead(PINVOLTAGESENSOR) == VoltageSensorON && brewDetected == 0 ) 
      {
        debugStream.writeI("HW Brew - Voltage Sensor -  Start") ;
        timeBrewdetection = millis() ;
        startZeit = millis() ;
        timerBrewdetection = 1 ;
        brewDetected = 1;
        lastbezugszeit = 0 ;
      }
    }
  }
}

/********************************************************
  after ~28 cycles the input is set to 99,66% if the real input value
  sum of inX and inY multiplier must be 1
  increase inX multiplier to make the filter faster
*****************************************************/
int filter(int input) {
  inX = input * 0.3;
  inY = inOld * 0.7;
  inSum = inX + inY;
  inOld = inSum;

  return inSum;
}


/********************************************************
    Timer 1 - ISR for PID calculation and heat realay output
******************************************************/

 #include "ISR.h"  

/********************************************************
    MQTT Callback Function: set Parameters through MQTT
******************************************************/


void mqtt_callback(char* topic, byte* data, unsigned int length) {
  char topic_str[255];
  os_memcpy(topic_str, topic, sizeof(topic_str));
  topic_str[255] = '\0';
  char data_str[length+1];
  os_memcpy(data_str, data, length);
  data_str[length] = '\0';
  char topic_pattern[255];
  char configVar[120];
  char cmd[64];
  double data_double;
  int data_int;



 // DEBUG_print("mqtt_parse(%s, %s)\n", topic_str, data_str);
  snprintf(topic_pattern, sizeof(topic_pattern), "%s%s/%%[^\\/]/%%[^\\/]", mqtt_topic_prefix, hostname);
  DEBUG_println(topic_pattern);
  if ( (sscanf( topic_str, topic_pattern , &configVar, &cmd) != 2) || (strcmp(cmd, "set") != 0) ) {
  DEBUG_print(topic_str);
    return;
  }
  DEBUG_println(topic_str);
  DEBUG_println(data_str);
  if (strcmp(configVar, "BrewSetPoint") == 0) {
    sscanf(data_str, "%lf", &data_double);
    mqtt_publish("BrewSetPoint", number2string(BrewSetPoint));
    if (Blynk.connected()) { Blynk.virtualWrite(V7, String(data_double));}
    BrewSetPoint = data_double;
    return;
  }
  if (strcmp(configVar, "brewtime") == 0) {
    sscanf(data_str, "%lf", &data_double);
    if (Blynk.connected()) { Blynk.virtualWrite(V8, String(data_double));}
    mqtt_publish("brewtime", number2string(brewtime/1000));
    brewtime = data_double * 1000 ;
    return;
  }
  if (strcmp(configVar, "preinfusion") == 0) {
    sscanf(data_str, "%lf", &data_double);
    if (Blynk.connected()) { Blynk.virtualWrite(V9, String(data_double));}
    mqtt_publish("preinfusion", number2string(preinfusion/1000));
    preinfusion = data_double * 1000;
    return;
  }
  if (strcmp(configVar, "preinfusionpause") == 0) {
    sscanf(data_str, "%lf", &data_double);
    if (Blynk.connected()) { Blynk.virtualWrite(V10, String(data_double));}
    mqtt_publish("preinfusionpause", number2string(preinfusionpause/1000));
    preinfusionpause = data_double * 1000;
    return;
  }
    if (strcmp(configVar, "pidON") == 0) {
    sscanf(data_str, "%lf", &data_double);
    if (Blynk.connected())  { Blynk.virtualWrite(V13,String(data_double));}
    mqtt_publish("pidON", number2string(pidON));
    pidON = data_double ;
    return;
  }

}
/*******************************************************
  Trigger for E-Silvia
*****************************************************/
//unsigned long previousMillisETrigger ;  // initialisation at the end of init()
//const unsigned long intervalETrigger = ETriggerTime ; // in Seconds
void ETriggervoid() 
{
  //Static variable only one time is 0 
  static int ETriggeractive = 0;
  unsigned long currentMillisETrigger = millis();
  if (ETRIGGER == 1) // E Trigger is active from userconfig
  { 
    // 
    if (currentMillisETrigger - previousMillisETrigger >= (1000*intervalETrigger))  //s to ms * 1000
    {  // check 
      ETriggeractive = 1 ;
      previousMillisETrigger = currentMillisETrigger;

      digitalWrite(PINETRIGGER, relayETriggerON);
    }
    // 10 Seconds later
    else if (ETriggeractive == 1 && previousMillisETrigger+(10*1000) < (currentMillisETrigger))
    {
    digitalWrite(PINETRIGGER, relayETriggerOFF);
    ETriggeractive = 0;
    }
  } 
}
  /********************************************************
   SteamON & Quickmill
  ******************************************************/
void checkSteamON() 
{
// check digital GIPO  
  if (digitalRead(STEAMONPIN) == HIGH) 
  {
    SteamON = 1;
  } 
  if (digitalRead(STEAMONPIN) == LOW && SteamFirstON == 0) // if via blynk on, then SteamFirstON == 1, prevent override
  {
    SteamON = 0;
  }
  /*  monitor QuickMill thermoblock steam-mode*/
  if (machine == QuickMill )
  {
    if (steamQM_active == true) 
    {
      if( checkSteamOffQM() == true ) 
      { // if true: steam-mode can be turned off
        SteamON = 0;
        steamQM_active = false;
        lastTimePVSwasON = 0;
      } 
      else
      {
        SteamON = 1;
      }
    }
  }
  if (SteamON == 1) 
  {
    setPoint = SteamSetPoint ;
  }
   if (SteamON == 0) 
  {
    setPoint = BrewSetPoint ;
  }
}

void setEmergencyStopTemp()
{
  if (machinestate == 40 || machinestate == 45) 
  {
    if (EmergencyStopTemp != 145)
    EmergencyStopTemp = 145;  
  }
  else
  {
    if (EmergencyStopTemp != 120)
    EmergencyStopTemp = 120;  
  }
}


void initSteamQM() 
{
  /*
    Initialize monitoring for steam switch off for QuickMill thermoblock
  */
  lastTimePVSwasON = millis(); // time when pinvoltagesensor changes from ON to OFF
  steamQM_active = true;
  timePVStoON = 0;
  SteamON = 1;
}

boolean checkSteamOffQM()
{
  /* 
    Monitor pinvoltagesensor during active steam mode of QuickMill thermoblock.
    Once the pinvolagesenor remains OFF for longer than a pump-pulse time peride 
    the switch is turned off and steam mode finished.
  */
  if( digitalRead(PINVOLTAGESENSOR) == VoltageSensorON ) {
    lastTimePVSwasON = millis();
  }

  if( (millis() - lastTimePVSwasON) > minPVSOffTimedForSteamModeQM_OFF ) {
    lastTimePVSwasON = 0;
    return true;
  }
  
  return false;
}

/********************************************************
   machinestatevoid
******************************************************/


void machinestatevoid() 
{
  /* 
  00 = init
  10 = kaltstart
  19 = Setpoint -1 Celsius
  20 = Setpoint überschritten, idle at setpoint
  30 = Bezug
  31 = Anzeige des Shottimers nach Bezugsende  
  35 = Nachlauf BD
  40 = Dampf
  45 = Cooling Flush nach Dampf
  50 = Backflush
  80 = Emergency Stop
  90 = PID Offline
  100 = Sensorerror
  */
  //DEBUG_println(machinestate);
  switch (machinestate) 
  {
    // init
    case 0: 
      if (Input < (BrewSetPoint-1) && millis() > 30*1000 ) // After 30 sec
       {
        machinestate = 10 ; // kaltstart
      }
      if (Input >= (BrewSetPoint-1) && millis() > 30*1000 ) // After 30 sec
      {
        machinestate = 19 ; // machine is hot, jump to other state
      }
      
      if (pidON == 0)
      {
        machinestate = 90 ; // offline
      }
     if(sensorError)
      {
        machinestate = 100 ;// sensorerror
      }
    break;

     // kaltstart
    case 10: 
      if (Input >= (BrewSetPoint-1) )
      {
        machinestate = 19 ;
      }
      if (SteamON == 1)
      {
        machinestate = 40 ; // Steam
      }

      if
      (
       (bezugsZeit > 0 && ONLYPID == 1) || // Bezugszeit bei Only PID  
       (ONLYPID == 0 && brewcounter > 10 && brewcounter <= 42) 
      )
      
      {
        machinestate = 30 ; // Brew
      }

      if (SteamON == 1)
      {
        machinestate = 40 ; // switch to  Steam
      }

      if (backflushON || backflushState > 10) 
      {
        machinestate = 50 ; // backflushON
      }

     if (pidON == 0)
      {
        machinestate = 90 ; // offline
      }
     if(sensorError)
      {
        machinestate = 100 ;// sensorerror
      }
      // Setpoint -1 Celsius
      case 19: 
      if (Input >= (BrewSetPoint))
      {
        machinestate = 20 ;
      }
      if
      (
       (bezugsZeit > 0 && ONLYPID == 1) || // Bezugszeit bei Only PID  
       (ONLYPID == 0 && brewcounter > 10 && brewcounter <= 42) 
      )
      {
        machinestate = 30 ; // Brew
      }
      if (SteamON == 1)
      {
        machinestate = 40 ; // Steam
      }

      if (backflushON || backflushState > 10) 
      {
        machinestate = 50 ; // backflushON
      }

      if (SteamON == 1)
      {
        machinestate = 40 ; // switch to  Steam
      }
    
     if (pidON == 0)
      {
        machinestate = 90 ; // offline
      }
     if(sensorError)
      {
        machinestate = 100 ;// sensorerror
      }  
    break;
    // normal PID
    case 20: 
      brewdetection();  //if brew detected, set PID values
      if
      (
       (bezugsZeit > 0 && ONLYPID == 1) || // Bezugszeit bei Only PID  
       (ONLYPID == 0 && brewcounter > 10 && brewcounter <= 42) 
      )
      {
        machinestate = 30 ; // Brew
      }
      if (SteamON == 1)
      {
        machinestate = 40 ; // Steam
      }

      if (backflushON || backflushState > 10) 
      {
        machinestate = 50 ; // backflushON
      }
      if (emergencyStop)
      {
        machinestate = 80 ; // Emergency Stop
      }
     if (pidON == 0)
      {
        machinestate = 90 ; // offline
      }
     if(sensorError)
      {
        machinestate = 100 ;// sensorerror
      }
    break;
     // Brew
    case 30:
      brewdetection();  
      // Ausgabe waehrend des Bezugs von Bruehzeit, Temp und heatrateaverage
      if (logbrew.check())
          debugStream.writeV("(tB,T,hra) --> %5.2f %6.2f %8.2f",(double)(millis() - startZeit)/1000,Input,heatrateaverage);
      if
      (
       (bezugsZeit > 35*1000 && Brewdetection == 1 && ONLYPID == 1  ) ||  // 35 sec later and BD PID active SW Solution
       (bezugsZeit == 0      && Brewdetection == 3 && ONLYPID == 1  ) ||  // Voltagesensor reset bezugsZeit == 0
       ((brewcounter == 10 || brewcounter == 43)   && ONLYPID == 0  ) // After brew
      )
      {
       if ((ONLYPID == 1 && Brewdetection == 3) || ONLYPID == 0 ) // only delay of shotimer for voltagesensor or brewcounter
       {
         machinestate = 31 ;
         lastbezugszeitMillis = millis() ; // for delay
        
       }
       if (ONLYPID == 1 && Brewdetection == 1 && timerBrewdetection == 1) //direct to PID BD
       {
         machinestate = 35 ;
       }
      } 
      if (SteamON == 1)
      {
        machinestate = 40 ; // Steam
      }

      if (emergencyStop)
      {
        machinestate = 80 ; // Emergency Stop
      }
     if (pidON == 0)
      {
        machinestate = 90 ; // offline
      }
     if(sensorError)
      {
        machinestate = 100 ;// sensorerror
      }
    break;
    // Sec after shot finish
    case 31: //lastbezugszeitMillis
    brewdetection();  
      if ( millis()-lastbezugszeitMillis > BREWSWITCHDELAY )
      {
       debugStream.writeI("Bezugsdauer: %4.1f s",lastbezugszeit/1000);
       machinestate = 35 ;
       lastbezugszeit = 0 ;
      }

      if (SteamON == 1)
      {
        machinestate = 40 ; // Steam
      }

     if (backflushON || backflushState > 10) 
      {
        machinestate = 50 ; // backflushON
      }

      if (emergencyStop)
      {
        machinestate = 80 ; // Emergency Stop
      }

     if (pidON == 0)
      {
        machinestate = 90 ; // offline
      }

     if(sensorError)
      {
        machinestate = 100 ;// sensorerror
      }
    break;
    // BD PID
    case 35:
    brewdetection();  
      if (timerBrewdetection == 0)
      {
        machinestate = 20 ; // switch to normal PID
      }
      if
      (
       (bezugsZeit > 0 && ONLYPID == 1  && Brewdetection == 3) || // New Brew inner BD only by Only PID AND Voltage Sensor
       (ONLYPID == 0 && brewcounter > 10 && brewcounter <= 42) 
      )
      {
        machinestate = 30 ; // Brew
      }
      
      if (SteamON == 1)
      {
        machinestate = 40 ; // switch to  Steam
      }

      if (backflushON || backflushState > 10) 
      {
        machinestate = 50 ; // backflushON
      }

      if (emergencyStop)
      {
        machinestate = 80 ; // Emergency Stop
      }
      if (pidON == 0)
      {
        machinestate = 90 ; // offline
      }
     if(sensorError)
      {
        machinestate = 100 ;// sensorerror
      }
    break;
    // Steam
    case 40:
      if (SteamON == 0)
      {
        machinestate = 45 ; //  switch to cool down after steam
      }

       if (emergencyStop)
      {
        machinestate = 80 ; // Emergency Stop
      }

      if (backflushON || backflushState > 10) 
      {
        machinestate = 50 ; // backflushON
      }

      if (pidON == 0)
      {
        machinestate = 90 ; // offline
      }
      if(sensorError)
      {
        machinestate = 100 ;// sensorerror
      }
    break;  

    case 45: // chill-mode after steam
    if (Brewdetection == 2 || Brewdetection == 3) 
      {
        /*
          Bei QuickMill Dampferkennung nur ueber Bezugsschalter moeglich, durch Aufruf von 
          brewdetection() kann neuer Dampfbezug erkannt werden
          */ 
        brewdetection();
      }
      if (Brewdetection == 1 && ONLYPID == 1)
      {
        // Ab lokalen Minumum wieder freigeben für state 20, dann wird bist Solltemp geheizt.
         if (heatrateaverage > 0 && Input < BrewSetPoint + 2) 
         {
            machinestate = 20;
         } 
      }
      if ((Brewdetection == 3 || Brewdetection == 2) && Input < BrewSetPoint + 2) 
      {
        machinestate = 20; //  switch to normal
      }

      if (SteamON == 1)
      {
        machinestate = 40 ; // Steam
      }

      if (backflushON || backflushState > 10) 
      {
        machinestate = 50 ; // backflushON
      }

      if (emergencyStop)
      {
        machinestate = 80 ; // Emergency Stop
      }
      if (pidON == 0)
      {
        machinestate = 90 ; // offline
      }
      if(sensorError)
      {
        machinestate = 100 ;// sensorerror
      }
    break;

    case 50: 
    // Backflush
      if (backflushON == 0)
       {
         machinestate = 20 ; //  switch to normal
       }

      if (emergencyStop)
       {
         machinestate = 80 ; // Emergency Stop
       }
      if (pidON == 0)
       {
         machinestate = 90 ; // offline
       }
      if(sensorError)
       {
         machinestate = 100 ;// sensorerror
       }
    break;
    // emergencyStop 
    case 80: 
      if (!emergencyStop)
      {
        machinestate = 20 ; // normal PID
      }
      if (pidON == 0)
      {
        machinestate = 90 ; // offline
      }
      if(sensorError)
      {
        machinestate = 100 ;
      }
    break;
    // PID offline
    case 90: 
      if (pidON == 1)
      {
        if(kaltstart) 
        {
          machinestate = 10 ; // kaltstart 
        }
        else if(!kaltstart && (Input > (BrewSetPoint-10) )) // Input higher BrewSetPoint-10, normal PID
        {
          machinestate = 20 ; // normal PID
        } 
        else if (Input <= (BrewSetPoint-10) )
        {
          machinestate = 10 ; // Input higher BrewSetPoint-10, kaltstart
          kaltstart = true;
        }
      }

      if(sensorError)
      {
        machinestate = 100 ;
      }
    break;
    // sensor error
    case 100:
      machinestate = 100 ;
    break;
  } // switch case
  if (machinestate != lastmachinestate) { 
    debugStream.writeI("new machinestate: %i -> %i",lastmachinestate, machinestate);
    lastmachinestate = machinestate;
  }
} // end void

void debugVerboseOutput()
{
  static PeriodicTrigger trigger(10000);
  if(trigger.check()) 
  {
    debugStream.writeV("Tsoll=%5.1f  Tist=%5.1f Machinestate=%2i KP=%4.2f KI=%4.2f KD=%4.2f",BrewSetPoint,Input,machinestate,bPID.GetKp(),bPID.GetKi(),bPID.GetKd());
  }
}

void setup() {
  DEBUGSTART(115200);
  debugStream.setup();

  if (MQTT == 1) {
    //MQTT
    snprintf(topic_will, sizeof(topic_will), "%s%s/%s", mqtt_topic_prefix, hostname, "will");
    snprintf(topic_set, sizeof(topic_set), "%s%s/+/%s", mqtt_topic_prefix, hostname, "set");
    mqtt.setServer(mqtt_server_ip, mqtt_server_port);
    mqtt.setCallback(mqtt_callback);
    checkMQTT();
  }

  /********************************************************
    Define trigger type
  ******************************************************/
  if (triggerType)
  {
    relayON = HIGH;
    relayOFF = LOW;
  } else {
    relayON = LOW;
    relayOFF = HIGH;
  }

  if (TRIGGERRELAYTYPE)
  {
    relayETriggerON = HIGH;
    relayETriggerOFF  = LOW;
  } else {
    relayETriggerON  = LOW;
    relayETriggerOFF  = HIGH;
  }
  if (VOLTAGESENSORTYPE)
  {
    VoltageSensorON = HIGH;
    VoltageSensorOFF  = LOW;
  } else {
    VoltageSensorON = LOW;
    VoltageSensorOFF  = HIGH;
  }

  /********************************************************
    Init Pins
  ******************************************************/
  pinMode(pinRelayVentil, OUTPUT);
  pinMode(pinRelayPumpe, OUTPUT);
  pinMode(pinRelayHeater, OUTPUT);
  pinMode(STEAMONPIN, INPUT);
  digitalWrite(pinRelayVentil, relayOFF);
  digitalWrite(pinRelayPumpe, relayOFF);
  digitalWrite(pinRelayHeater, LOW);
  if (ETRIGGER == 1) // IF Etrigger selected 
  { 
    pinMode(PINETRIGGER, OUTPUT);
  }
  if (BREWDETECTION == 3) // IF Voltage sensor selected 
  { 
    pinMode(PINVOLTAGESENSOR, PINMODEVOLTAGESENSOR);
  }
  if (PINBREWSWITCH > 0) // IF PINBREWSWITCH & Steam selected 
  { 
    #if (defined(ESP8266) && PINBREWSWITCH == 16) 
      pinMode(PINBREWSWITCH, INPUT_PULLDOWN_16);
    #endif
    #if (defined(ESP8266) && PINBREWSWITCH == 15) 
      pinMode(PINBREWSWITCH, INPUT);
    #endif
    #if defined(ESP32) 
      pinMode(PINBREWSWITCH, INPUT);//
    #endif
  }
    #if (defined(ESP8266) && STEAMONPIN == 16) 
      pinMode(STEAMONPIN, INPUT_PULLDOWN_16);
    #endif
      #if (defined(ESP8266) && STEAMONPIN == 15) 
    pinMode(STEAMONPIN, INPUT);
    #endif
    #if defined(ESP32) 
      pinMode(STEAMONPIN, INPUT_PULLDOWN);
    #endif
  /********************************************************
    DISPLAY 128x64
  ******************************************************/
  #if DISPLAY != 0
    u8g2.setI2CAddress(oled_i2c * 2);
    u8g2.begin();
    u8g2_prepare();
    displayLogo(sysVersion, "");
    delay(2000);
  #endif
   /********************************************************
    Init Scale by BREWMODE 2 or SHOTTIMER 2
  ******************************************************/
  #if (BREWMODE == 2 || ONLYPIDSCALE == 1)
    initScale() ;
  #endif


  /********************************************************
    VL530L0x TOF sensor
  ******************************************************/
  if (TOF != 0) { 
  lox.begin(tof_i2c); // initialize TOF sensor at I2C address
  lox.setMeasurementTimingBudgetMicroSeconds(2000000);
  }

  /********************************************************
     BLYNK & Fallback offline
  ******************************************************/
  if (Offlinemodus == 0) 
  {
    #if defined(ESP8266)
      WiFi.hostname(hostname);
    #endif
    unsigned long started = millis();
    #if DISPLAY != 0
      displayLogo(langstring_connectwifi1, ssid);
    #endif
    /* Explicitly set the ESP8266 to be a WiFi-client, otherwise, it by default,
      would try to act as both a client and an access-point and could cause
      network-issues with your other WiFi-devices on your WiFi-network. */
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);   //needed, otherwise exceptions are triggered \o.O/
    WiFi.begin(ssid, pass);
    #if defined(ESP32) // ESP32
     WiFi.setHostname(hostname); // for ESP32port
    #endif
    debugStream.writeI("Connecting to %s ...",ssid);

    // wait up to 20 seconds for connection:
    while ((WiFi.status() != WL_CONNECTED) && (millis() - started < 20000))
    {
      yield();    //Prevent Watchdog trigger
    }

    checkWifi();    //try to reconnect

    if (WiFi.status() == WL_CONNECTED)
    {
      debugStream.writeI("WiFi connected - IP = %i.%i.%i.%i",WiFi.localIP()[0],WiFi.localIP()[1],WiFi.localIP()[2],WiFi.localIP()[3]);
      debugStream.writeI("Wifi works, now try Blynk (timeout 30s)");
      if (fallback == 0) {
        #if DISPLAY != 0
          displayLogo(langstring_connectblynk1[0], langstring_connectblynk1[1]);
        #endif
      } else if (fallback == 1) {
        #if DISPLAY != 0
          displayLogo(langstring_connectwifi2[0], langstring_connectwifi2[1]);
        #endif
      }
      delay(1000);

      //try blynk connection
      Blynk.config(auth, blynkaddress, blynkport) ;
      Blynk.connect(30000);

      if (Blynk.connected() == true) 
      {
        #if DISPLAY != 0
          displayLogo(langstring_connectblynk2[0], langstring_connectblynk2[1]);
        #endif
        debugStream.writeI("Blynk is online");
        if (fallback == 1) 
        {
          debugStream.writeI("sync all variables and write new values to eeprom");
          // Blynk.run() ;
          Blynk.syncVirtual(V4);
          Blynk.syncVirtual(V5);
          Blynk.syncVirtual(V6);
          Blynk.syncVirtual(V7);
          Blynk.syncVirtual(V8);
          Blynk.syncVirtual(V9);
          Blynk.syncVirtual(V10);
          Blynk.syncVirtual(V11);
          Blynk.syncVirtual(V12);
          Blynk.syncVirtual(V13);
          Blynk.syncVirtual(V14);
          Blynk.syncVirtual(V15);
          Blynk.syncVirtual(V30);
          Blynk.syncVirtual(V31);
          Blynk.syncVirtual(V32);
          Blynk.syncVirtual(V33);
          Blynk.syncVirtual(V34);
          // Blynk.syncAll();  //sync all values from Blynk server
          // Werte in den eeprom schreiben
          // ini eeprom mit begin
          EEPROM.begin(1024);
          EEPROM.put(0, aggKp);
          EEPROM.put(10, aggTn);
          EEPROM.put(20, aggTv);  
          EEPROM.put(30, BrewSetPoint);
          EEPROM.put(40, brewtime);
          EEPROM.put(50, preinfusion);
          EEPROM.put(60, preinfusionpause);
          EEPROM.put(90, aggbKp);
          EEPROM.put(100, aggbTn);
          EEPROM.put(110, aggbTv);
          EEPROM.put(120, brewtimersoftware);
          EEPROM.put(130, brewboarder);
          // eeprom schließen
          EEPROM.commit();
        }
      } else 
      {
        debugStream.writeI("No connection to Blynk");
        EEPROM.begin(1024);  // open eeprom
        double dummy; // check if eeprom values are numeric (only check first value in eeprom)
        EEPROM.get(0, dummy);
        debugStream.writeI("check eeprom 0x00 in dummy: %f",dummy);
        if (!isnan(dummy)) 
        {
          #if DISPLAY != 0
           displayLogo("3: Blynk not connected", "use eeprom values..");
          #endif 
          EEPROM.get(0, aggKp);
          EEPROM.get(10, aggTn);
          EEPROM.get(20, aggTv);
          EEPROM.get(30, BrewSetPoint);
          EEPROM.get(40, brewtime);
          EEPROM.get(50, preinfusion);
          EEPROM.get(60, preinfusionpause);
          EEPROM.get(90, aggbKp);
          EEPROM.get(100, aggbTn);
          EEPROM.get(110, aggbTv);
          EEPROM.get(120, brewtimersoftware);
          EEPROM.get(130, brewboarder);
        } 
      }
    }
    else 
    { 
      #if DISPLAY != 0
        displayLogo(langstring_nowifi[0], langstring_nowifi[1]); 
      #endif
      debugStream.writeI("No WIFI");
      WiFi.disconnect(true);
      delay(1000);
    }
  }

  /********************************************************
     OTA
  ******************************************************/
  if (ota && Offlinemodus == 0 && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.setHostname(OTAhost);  //  Device name for OTA
    ArduinoOTA.setPassword(OTApass);  //  Password for OTA
    ArduinoOTA.begin();
  }


  /********************************************************
     Ini PID
  ******************************************************/

  //setPointTemp = BrewSetPoint;
  bPID.SetSampleTime(windowSize);
  bPID.SetOutputLimits(0, windowSize);
  bPID.SetMode(AUTOMATIC);


  /********************************************************
     TEMP SENSOR
  ******************************************************/
  if (TempSensor == 1) {
    sensors.begin();
    sensors.getAddress(sensorDeviceAddress, 0);
    sensors.setResolution(sensorDeviceAddress, 10) ;
    sensors.requestTemperatures();
    Input = sensors.getTempCByIndex(0);
  }

  if (TempSensor == 2) {
    temperature = 0;
    #if (ONE_WIRE_BUS == 16 && defined(ESP8266))
         Sensor1.getTemperature(&temperature);
         Input = Sensor1.calc_Celsius(&temperature);
    #endif
    #if ((ONE_WIRE_BUS != 16 && defined(ESP8266)) || defined(ESP32))
        Input = Sensor2.getTemp();
     #endif
  }

  /********************************************************
    movingaverage ini array
  ******************************************************/
  if (Brewdetection == 1) {
    for (int thisReading = 0; thisReading < numReadings; thisReading++) {
      readingstemp[thisReading] = 0;
      readingstime[thisReading] = 0;
      readingchangerate[thisReading] = 0;
    }
  }
  /*
  if (TempSensor == 2) {
    temperature = 0;
    #if (ONE_WIRE_BUS == 16)
         Sensor1.getTemperature(&temperature);
         Input = Sensor1.calc_Celsius(&temperature);
    #endif
    #if (ONE_WIRE_BUS != 16)
        Input = Sensor2.getTemp();
     #endif
  } */

  //Initialisation MUST be at the very end of the init(), otherwise the time comparision in loop() will have a big offset
  unsigned long currentTime = millis();
  previousMillistemp = currentTime;
  windowStartTime = currentTime;
  previousMillisDisplay = currentTime;
  previousMillisBlynk = currentTime;
  previousMillisETrigger = currentTime; 
  previousMillisVoltagesensorreading = currentTime;
  #if (BREWMODE ==  2) 
  previousMillisScale = currentTime;
  #endif
  #if (PRESSURESENSOR == 1)
  previousMillisPressure = currentTime;
  #endif
  setupDone = true;

  #if defined(ESP8266) 
    /********************************************************
      Timer1 ISR - Initialisierung
      TIM_DIV1 = 0,   //80MHz (80 ticks/us - 104857.588 us max)
      TIM_DIV16 = 1,  //5MHz (5 ticks/us - 1677721.4 us max)
      TIM_DIV256 = 3  //312.5Khz (1 tick = 3.2us - 26843542.4 us max)
    ******************************************************/
      timer1_isr_init();
      timer1_attachInterrupt(onTimer1ISR);
      //timer1_enable(TIM_DIV16, TIM_EDGE, TIM_SINGLE);
      //timer1_write(50000); // set interrupt time to 10ms
      timer1_enable(TIM_DIV256, TIM_EDGE, TIM_SINGLE);
      timer1_write(6250); // set interrupt time to 20ms
  #endif
  #if defined(ESP32) // ESP32
        /********************************************************
    Timer1 ISR - Initialisierung
    TIM_DIV1 = 0,   //80MHz (80 ticks/us - 104857.588 us max)
    TIM_DIV16 = 1,  //5MHz (5 ticks/us - 1677721.4 us max)
    TIM_DIV256 = 3  //312.5Khz (1 tick = 3.2us - 26843542.4 us max)
  ******************************************************/
    timer = timerBegin(0, 80, true); //m
    timerAttachInterrupt(timer, &onTimer, true);//m
    timerAlarmWrite(timer, 10000, true);//m
    timerAlarmEnable(timer);//m
  #endif
  
}
void loop() {
  if (calibration_mode == 1 && TOF == 1) {
      loopcalibrate();
  } else {
      looppid();
      debugStream.handle();
      debugVerboseOutput();
    }
}

// TOF Calibration_mode 
void loopcalibrate() 
{
    //Deactivate PID
  if (pidMode == 1) 
  {
    pidMode = 0;
    bPID.SetMode(pidMode);
    Output = 0 ;false;
  }
  if (Blynk.connected()) 
  {  // If connected run as normal
      Blynk.run();
      blynkReCnctCount = 0; //reset blynk reconnects if connected
  } else  
  {
    checkBlynk();
  }
    digitalWrite(pinRelayHeater, LOW); //Stop heating to be on the safe side ...

  unsigned long currentMillisTOF = millis();
  if (currentMillisTOF - previousMillisTOF >= intervalTOF) 
  {
    previousMillisTOF = millis() ;
    VL53L0X_RangingMeasurementData_t measure;  //TOF Sensor measurement
    lox.rangingTest(&measure, false); // pass in 'true' to get debug data printout!
    distance = measure.RangeMilliMeter;  //write new distence value to 'distance'
    DEBUG_print(distance);
    DEBUG_println("mm");
    #if DISPLAY !=0
        displayDistance(distance);
    #endif
  }  
}


void looppid() 
{
  //Only do Wifi stuff, if Wifi is connected
  if (WiFi.status() == WL_CONNECTED && Offlinemodus == 0) 
  { 
    //MQTT
    if (MQTT == 1) 
    {
      checkMQTT();
      if (mqtt.connected() == 1)
      {
        mqtt.loop();
      }
    }
    ArduinoOTA.handle();  // For OTA
    // Disable interrupt it OTA is starting, otherwise it will not work
    ArduinoOTA.onStart([]() 
    {
      
      #if defined(ESP8266) 
      timer1_disable();
      #endif
      #if defined(ESP32) 
      timerAlarmDisable(timer);
      #endif
      digitalWrite(pinRelayHeater, LOW); //Stop heating
    });
    ArduinoOTA.onError([](ota_error_t error) 
    {
      #if defined(ESP8266) 
      timer1_enable(TIM_DIV16, TIM_EDGE, TIM_SINGLE);
      #endif
      #if defined(ESP32) 
      timerAlarmEnable(timer);
      #endif
    });
    // Enable interrupts if OTA is finished
    ArduinoOTA.onEnd([]() 
    {
      #if defined(ESP8266) 
       timer1_enable(TIM_DIV16, TIM_EDGE, TIM_SINGLE);
      #endif
      #if defined(ESP32)
        timerAlarmEnable(timer);
      #endif
    });

    if (Blynk.connected()) 
    {  // If connected run as normal
      Blynk.run();
      blynkReCnctCount = 0; //reset blynk reconnects if connected
    } else  
    {
      checkBlynk();
    }
    wifiReconnects = 0;   //reset wifi reconnects if connected
  } else 
  {
    checkWifi();
  }
  if (TOF != 0) 
  {
        unsigned long currentMillisTOF = millis();
      if (currentMillisTOF - previousMillisTOF >= intervalTOF) 
      {
        previousMillisTOF = millis() ;
        VL53L0X_RangingMeasurementData_t measure;  //TOF Sensor measurement
        lox.rangingTest(&measure, false); // pass in 'true' to get debug data printout!
        distance = measure.RangeMilliMeter;  //write new distence value to 'distance'
        if (distance <= 1000)
        {
          percentage = (100.00 / (water_empty - water_full)) * (water_empty - distance); //calculate percentage of waterlevel
          DEBUG_println(percentage);
        }
      }
  }
  // voids
  refreshTemp();   //read new temperature values
  testEmergencyStop();  // test if Temp is to high
  #if (BREWMODE == 2 || ONLYPIDSCALE == 1 )
    checkWeight() ; // Check Weight Scale in the loop
  #endif
    #if (PRESSURESENSOR == 1)
    checkPressure();
    #endif
  brew();   //start brewing if button pressed
  checkSteamON(); // check for steam
  setEmergencyStopTemp();
  sendToBlynk();
  machinestatevoid() ; // calc machinestate
  if (ETRIGGER == 1) // E-Trigger active then void Etrigger() 
  { 
    ETriggervoid();
  }  
  #if (ONLYPIDSCALE == 1) // only by shottimer 2, scale
      shottimerscale() ;
  #endif


  //check if PID should run or not. If not, set to manuel and force output to zero
  // OFFLINE
  //voids Display & BD
  #if DISPLAY != 0
      unsigned long currentMillisDisplay = millis();
      if (currentMillisDisplay - previousMillisDisplay >= 100) 
      {
        displayShottimer() ;
      }
      if (currentMillisDisplay - previousMillisDisplay >= intervalDisplay)
      {
        previousMillisDisplay = currentMillisDisplay;
        #if DISPLAYTEMPLATE < 20 // not in vertikal template
          Displaymachinestate() ;
        #endif
        printScreen();  // refresh display
      }
  #endif
  if (machinestate == 90 || machinestate == 100 || machinestate == 80) // Offline see machinestate.h
  {
    if (pidMode == 1)
    { 
      // Force PID shutdown
      pidMode = 0;
      bPID.SetMode(pidMode);
      Output = 0 ;
      digitalWrite(pinRelayHeater, LOW); //Stop heating
    }
  } 
  else // no sensorerror, no pid off or no Emergency Stop
  {
    if (pidMode == 0)
    {
    pidMode = 1;
    bPID.SetMode(pidMode);
    }
  }

  //Set PID if first start of machine detected, and no SteamON
  if (machinestate == 0 || machinestate == 10 || machinestate == 19) // Cold Start states 
  {
    if (startTn != 0) {
      startKi = startKp / startTn;
    } else {
      startKi = 0 ;
    }
    bPID.SetTunings(startKp, startKi, 0, P_ON_M);
  // normal PID
  } 
  if (machinestate == 20 ) 
  {    //Prevent overwriting of brewdetection values
    // calc ki, kd
    if (aggTn != 0) {
      aggKi = aggKp / aggTn ;
    } else {
      aggKi = 0 ;
    }
    aggKd = aggTv * aggKp ;
    bPID.SetTunings(aggKp, aggKi, aggKd, PonE);
    kaltstart = false;
  }
  // BD PID
  if (machinestate >= 30 && machinestate <= 35)  
  {
    // calc ki, kd
    if (aggbTn != 0) {
      aggbKi = aggbKp / aggbTn ;
    } else {
      aggbKi = 0 ;
    }
    aggbKd = aggbTv * aggbKp ;
    bPID.SetTunings(aggbKp, aggbKi, aggbKd, PonE) ;
  }
  // Steam on
  if (machinestate == 40) // STEAM
  {
    // if (aggTn != 0) {
    //   aggKi = aggKp / aggTn ;
    // } else {
    //   aggKi = 0 ;
    // }
    // aggKi = 0 ;
    // aggKd = aggTv * aggKp ;
    bPID.SetTunings(150, 0, 0, PonE);
  }

  if (machinestate == 45) // chill-mode after steam
  {
    switch (machine) {
      
      case QuickMill:
        aggbKp = 150;
        aggbKi = 0;
        aggbKd = 0;
      break;
      
      default:
        // calc ki, kd
        if (aggbTn != 0) {
          aggbKi = aggbKp / aggbTn;
        } else {
          aggbKi = 0;
        }
        aggbKd = aggbTv * aggbKp;
    }

    bPID.SetTunings(aggbKp, aggbKi, aggbKd, PonE) ;
  }  
  //sensor error OR Emergency Stop
}
