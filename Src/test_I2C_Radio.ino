
#include <Arduino.h>
#include <Wire.h>

#include <radio.h>
#include <RDA5807M.h>

#define FIX_BAND RADIO_BAND_FM                                                                                     /// The band that will be tuned by this sketch is FM.
#define FIX_STATION 10400                                                                                          /// The station that will be tuned by this sketch is 104.0 MHz.
#define FIX_VOLUME 13                                                                                              /// The volume that will be set by this sketch is level 13 .
const int buttonPin = 3;                                                                                           /// GPIO pin connected to the input button
RDA5807M radio;                                                                                                    /// Create an instance of Class for RDA5807M Chip
int count = 0;                                                                                                     /// Counter variable

unsigned long previousMillis = 0;                                                                                  /// Variable to store the previous time
const long interval = 3000;                                                                                        /// Interval (in milliseconds) for the delay




int radio_station[10]={10620,9270,9350,9430,9830,10130,10260,10400,10480,10640}; 
                                                                                                                   /// Setup a FM only radio configuration
                                                                                                                   /// with some debugging on the Serial port
void setup() {
                                                                                                                   /// open the Serial port
  Serial.begin(115200);
  Wire.begin(); //comment in case of esp01 @infinity
//  Wire.begin(2,0); //uncomment in case of esp01 @infinity
  pinMode(buttonPin, INPUT_PULLUP);  
                                                                                                                   /// Set button pin as input with pull-up resistor
  Serial.println("RDA5807M Radio...");
  delay(200);

                                                                                                                   /// Enable information to the Serial port
  radio.debugEnable(true);
  radio._wireDebug(false);

                                                                                                                   /// Initialize the Radio
  if (!radio.initWire(Wire)) {
  Serial.println("no radio chip found.");
  delay(4000);
//  ESP.restart();
  };
                                                                  Serial.println(count);
                                                                  Serial.println(radio_station[count]);
  // Set all radio setting to the fixed values.
  radio.setBandFrequency(FIX_BAND, radio_station[count]);
  station_call_function();
  radio.setVolume(FIX_VOLUME);
  radio.setMono(false);
  radio.setMute(false);
}                                 /// setup


                                  /// show the current chip data every 3 seconds.
void loop() { 
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {                                                               /// Check if the interval has elapsed
  
  station_call_function();  
  previousMillis = currentMillis;                                                                                 /// Update the previous time for the next interval
  }
  if (digitalRead(buttonPin) == LOW) {                                                                            /// Check if button is pressed (LOW state due to pull-up resistor)
  count++;                                                                                                        /// Increment count
  if (count > 10) {                                                                                               /// Check if count is greater than 10
  count = 0;                                                                                                      /// Reset count to 0
  radio.setBandFrequency(FIX_BAND, radio_station[count]);
  }  
  radio.setBandFrequency(FIX_BAND, radio_station[count]);
  
  delay(1500);                                                                                                    /// Delay for readability (adjust as needed)
      
  }

}                    

int station_call_function(){
  char s[12];
  radio.formatFrequency(s, sizeof(s));
  Serial.print("Station:");
  Serial.println(s);

  Serial.print("Radio:");
  radio.debugRadioInfo();

  Serial.print("Audio:");
  radio.debugAudioInfo();
  return 0;
}

                                                                                                                /// End.
