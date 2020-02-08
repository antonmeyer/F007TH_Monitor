/*
 Andrew's sketch for capturing data from Ambient F007th Thermo-Hygrometer and uploading it to Xively

 Inspired by the many weather station hackers who have gone before,
 Only possible thanks to the invaluable help and support on the Arduino forums.

 With particular thanks to

 Rob Ward (whose Manchester Encoding reading by delay rather than interrupt
 is the basis of this code)
 https://github.com/robwlakes/ArduinoWeatherOS

 The work of 3zero8 capturing and analysing the F007th data
 http://forum.arduino.cc/index.php?topic=214436.0

 The work of Volgy capturing and analysing the F007th data
 https://github.com/volgy/gr-ambient

 Marco Schwartz for showing how to send sensor data to websites
 http://www.openhomeautomation.net/

 The forum contributions of;
   dc42: showing how to create 5 minute samples.
   jremington: suggesting how to construct error checking using averages (although not used this is where the idea of using newtemp and newhum for error checking originated)
   Krodal: for his 6 lines of code in the forum thread "Compare two sensor values"

 This example code is in the public domain.

 What this code does:
   Captures Ambient F007th Thermo-Hygrometer data packets by;
     Identifying a header of at least 10 rising edges (manchester encoding binary 1s)
     Synchs the data correctly within byte boundaries
     Distinguishes between F007th data packets and other 434Mhz signals with equivalent header by checking value of sensor ID byte
   Correctly identifies positive and negative temperature values to 1 decimal place for up to 8 channels
   Correctly identifies humidity values for up to 8 channels
   Error checks data by rejecting;
     humidity value outside the range 1 to 100%
     temperature changes of approx 1C per minute or greater
   Send the data by wifi to Xively every 5 minutes

Hardware to use with this code
  6 F007th Thermo-Hygrometer set to different channels (can be adapted for between 1 and 8)
  A 434Mhz receiver
  17cm strand of CAT-5 cable as an antenna.
  CC3000 (I use the Adafruit breakout board with ceramic antenna)

Code optimisation
   In order to improve reliability this code has been optimised to remove float values except when sending to Xively.
   This code does not provide any output to the serial monitor on what is happening,  see earlier versions for printouts.

F007th Ambient Thermo-Hygrometer
Sample Data:
0        1        2        3        4        5        6        7
FD       45       4F       04       4B       0B       52       0
0   1    2   3    4   5    6   7    8   9    A   B    C   D    E
11111101 01000101 01001111 00000100 01001011 00001011 01010010 0000
hhhhhhhh SSSSSSSS NRRRRRRR bCCCTTTT TTTTTTTT HHHHHHHH CCCCCCCC ????

Channel 1 F007th sensor displaying 21.1 Centigrade and 11% RH

hhhhhhhh = header with final 01 before data packet starts (note using this sketch the header 01 is omitted when the binary is displayed)
SSSSSSSS = sensor ID, F007th = Ox45
NRRRRRRR = Rolling Code Byte? Resets each time the battery is changed
b = battery indicator?
CCC = Channel identifier, channels 1 to 8 can be selected on the F007th unit using dipswitches. Channel 1 => 000, Channel 2 => 001, Channel 3 => 010 etc.
TTTT TTTTTTTT = 12 bit temperature data.
      To obtain F: convert binary to decimal, take away 400 and divide by 10 e.g. (using example above) 010001001011 => 1099
      (1099-400)/10= 69.9F
      To obtain C: convert binary to decimal, take away 720 and multiply by 0.0556 e.g.
      0.0556*(1099-720)= 21.1C
HHHHHHHH = 8 bit humidity in binary. e.g. (using example above) 00001011 => 11
CCCCCCCC = checksum? Note that this sketch only looks at the first 6 bytes and ignores the checksum

*/

// Interface Definitions

#include <Arduino.h>
#include <U8x8lib.h>

U8X8_SH1106_128X64_NONAME_HW_I2C oled(/* reset=*/U8X8_PIN_NONE);

int RxPin = 2; // The number of signal from the Rx

// Variables for Manchester Receiver Logic:
word sDelay = 242;         // Small Delay about 1/4 of bit duration
word lDelay = 484;         // Long Delay about 1/2 of bit duration, 1/4 + 1/2 = 3/4
byte polarity = 1;         // 0 for lo->hi==1 or 1 for hi->lo==1 for Polarity, sets tempBit at start
byte tempBit = 1;          // Reflects the required transition polarity
boolean firstZero = false; // flags when the first '0' is found.
// Variables for Header detection
byte headerBits = 10; // The number of ones expected to make a valid header
byte headerHits = 0;  // Counts the number of "1"s to determine a header
// Variables for Byte storage
boolean sync0In = true; // Expecting sync0 to be inside byte boundaries, set to false for sync0 outside bytes
byte dataByte = 0;      // Accumulates the bit information
byte nosBits = 6;       // Counts to 8 bits within a dataByte
byte maxBytes = 6;      // Set the bytes collected after each header. NB if set too high, any end noise will cause an error
byte nosBytes = 0;      // Counter stays within 0 -> maxBytes
// Variables for multiple packets
byte bank = 0;       // Points to the array of 0 to 3 banks of results from up to 4 last data downloads
byte nosRepeats = 3; // Number of times the header/data is fetched at least once or up to 4 times
// Banks for multiple packets if required (at least one will be needed)
byte manchester[7]; // Array to store 7 bytes of manchester pattern decoded on the fly

// Variables to prepare recorded values for Ambient

byte stnId = 0;   // Identifies the channel number
int dataType = 0; // Identifies the Ambient Thermo-Hygrometer code
int differencetemp = 0;
int differencehum = 0;
int Newtemp = 0;
int Newhum = 0;
int chTemp[8];
int chHum[8];
unsigned long chLastRecv[8];

char displaystr[20];
float siTemp;

volatile boolean isrcalled;
volatile unsigned long EdgeTime; // zur Speicherung der Zeit
unsigned long LastEdgeTime = 0;
unsigned long DiffEdgeTime = 0;
unsigned long Dstop = 0;
byte rxstate = 0;

void setup()
{
  Serial.begin(115200);
  Serial.println("hallo here is the 433MHz Monitor");

  oled.begin();
  //oled.setFont(u8x8_font_chroma48medium8_r);
  oled.setFont(u8x8_font_inr21_2x4_n);

  oled.drawString(0, 1, "12345");

  pinMode(RxPin, INPUT);
  eraseManchester(); // clear the array to different nos cause if all zeroes it might think that is a valid 3 packets ie all equal
  chTemp[0] = chTemp[1] = chTemp[2] = chTemp[3] = chTemp[4] = chTemp[5] = chTemp[6] = chTemp[7] = 720;
  chHum[0] = chHum[1] = chHum[2] = chHum[3] = chHum[4] = chHum[5] = chHum[6] = chHum[7] = 0;
  chLastRecv[0] = chLastRecv[1] = chLastRecv[2] = chLastRecv[3] = chLastRecv[4] = chLastRecv[5] = chLastRecv[6] = chLastRecv[7] = 0;

  pinMode(2, INPUT_PULLUP);
  attachInterrupt(0, isr, CHANGE); // interrupt 0 is pin 2
}

// Main RF, to find header, then sync in with it and get a packet.

// Interrupt Service Routine for a falling edge
void isr()
{
  if (!isrcalled)
  {
    EdgeTime = micros();
    isrcalled = true;
  }
} // end of isr

void loop()
{
  
  //ToDo init values depends on the state: new packet
  //state 0 = init = new packet
  //state 1 = wait for first change
  //state 2 = sDelay
  //state 3 = lDelay
  //state 4 = processBit

  //digitalWrite(7,LOW); //is for Oszi trigger

  

  switch (rxstate)
  {

  case 0:               // new packet
    tempBit = polarity; // these begin the same for a packet
    firstZero = false;
    headerHits = 0;
    nosBits = 6;
    nosBytes = 0;
    rxstate++; //next state
    isrcalled = false;
    //break;
  case 1: //waiting for edge
    if ((digitalRead(RxPin) == tempBit))
    {
      Dstop = EdgeTime + sDelay;
      //isrcalled = false; //double check this state handling race condition, noise, does it fit for the timing in all cases? 
      rxstate = 2;
    }
    else
      break;
  case 2: // sDelay
    if (micros() > Dstop)
    { // delay passed
      // 3/4 the way through, if RxPin has changed it is definitely an error
      if (digitalRead(RxPin) != tempBit)
      {
        rxstate = 0;
        break; // something has gone wrong, polarity has changed too early, ie always an error
      }        // exit and retry
      Dstop+=lDelay; //next stop for state 3
      rxstate = 3;
    } else break; //we have to wait in state 2

  case 3:
     if (micros() > Dstop) { 
       //delay passed
      // now 1 quarter into the next bit pattern,
      rxstate = 4;
     } else break; //we have to wait in state 3
  case 4:
    if (digitalRead(RxPin) == tempBit)
    { // if RxPin has not swapped, then bitWaveform is swapping
      // If the header is done, then it means data change is occuring ie 1->0, or 0->1
      // data transition detection must swap, so it loops for the opposite transitions
      tempBit = tempBit ^ 1;
    } // end of detecting no transition at end of bit waveform, ie end of previous bit waveform same as start of next bitwaveform

    //****************************//
    // Now process the tempBit state and make data definite 0 or 1's, allow possibility of Pos or Neg Polarity
    byte bitState = tempBit ^ polarity; // if polarity=1, invert the tempBit or if polarity=0, leave it alone.
    if (bitState == 1)
    { // 1 data could be header or packet
      if (!firstZero)
      {
        headerHits++;
      }
      else
      {
        add(bitState); // already seen first zero so add bit in
      }
    } // end of dealing with ones
    else
    { // bitState==0 could first error, first zero or packet
      // if it is header there must be no "zeroes" or errors
      if (headerHits < headerBits)
      {
        // Still in header checking phase, more header hits required
        rxstate = 0;
        break; // landing here means header is corrupted, so it is probably an error
      }        // end of detecting a "zero" inside a header
      else
      {
        firstZero = true;
        add(bitState);
      }          // end of dealing with a first zero
    }            // end of dealing with zero's (in header, first or later zeroes)
    rxstate = 1; //next bit
    isrcalled = false; //double check this state handling race condition, noise, does it fit for the timing in all cases? 
  }  // case statement
} // end of mainloop

// Read the binary data from the bank and apply conversions where necessary to scale and format data

void add(byte bitData)
{
  dataByte = (dataByte << 1) | bitData;
  nosBits++;
  if (nosBits == 8)
  {
    nosBits = 0;
    manchester[nosBytes] = dataByte;
    nosBytes++;
  }
  if (nosBytes == maxBytes)
  {
    rxstate = 0; // we got all bytes lets start for the next packet
    // Subroutines to extract data from Manchester encoding and error checking

    // Identify channels 0 to 7 by looking at 3 bits in byte 3
    int stnId = (manchester[3] & B01110000) / 16;

    // Identify sensor by looking for sensorID in byte 1 (F007th Ambient Thermo-Hygrometer = 0x45)
    dataType = manchester[1];

    // Gets raw temperature from bytes 3 and 4 (note this is neither C or F but a value from the sensor)
    Newtemp = float((manchester[3] & B00000111) * 256) + manchester[4];

    // Gets humidity data from byte 5
    Newhum = manchester[5];

    // Checks sensor is a F007th with a valid humidity reading equal or less than 100
    if (dataType == 0x45 && Newhum <= 100)
    {
      saveReading(stnId, Newtemp, Newhum);
    }
  }
}

void saveReading(int stnId, int newTemp, int newHum)
{
  bool sendData = false;
  if (stnId >= 0 && stnId <= 7)
  {

    // If the raw temperature is 720 (default when sketch started so first reading), accept the new readings as the temperature and humidity on channel 1
    if (chTemp[stnId] == 720)
    {
      chTemp[stnId] = newTemp;
      chHum[stnId] = newHum;
      sendData = true;
    }
    // If the raw temperature is other than 720 (so a subsequent reading), check that it is close to the previous reading before accepting as the new channel 1 reading
    else
    {
      differencetemp = newTemp - chTemp[stnId];
      differencehum = newHum - chHum[stnId];
      if ((differencetemp < 20 && differencetemp > -20) && (differencehum < 5 && differencehum > -5))
      {
        chTemp[stnId] = newTemp;
        chHum[stnId] = newHum;
        sendData = true;
      }
    }

    unsigned long now = millis();

    if (sendData && (chLastRecv[stnId] > now || (now - chLastRecv[stnId]) > 1000))
    {
      // checks above seems to avoid too many outpuut
      //digitalWrite(7,HIGH); //is for Oszi trigger
      //delay(1);

      Serial.print(stnId + 1);
      Serial.print(":");
      //Serial.print(newTemp - 400); // in F
      siTemp = 0.0556 * (newTemp - 720);
      Serial.print(siTemp);
      Serial.print(":");
      Serial.println(newHum);
      chLastRecv[stnId] = now;

      oled.clearDisplay();
      oled.drawGlyph(0,0, ('1'+stnId) );
      oled.drawUTF8(2,0,": ");
      char tempstr[6]; //float does not work in Arduino sprintf
      dtostrf(siTemp, 2, 1, tempstr);
      snprintf(displaystr, 15, "%s ", tempstr);
      oled.drawUTF8(4, 0, displaystr);
      snprintf(displaystr, 15, "%d  ", newHum);
      oled.drawUTF8(0, 4, displaystr);
    }
  }
}

void eraseManchester()
{
  for (int j = 0; j < 4; j++)
  {
    manchester[j] = j;
  }
}
