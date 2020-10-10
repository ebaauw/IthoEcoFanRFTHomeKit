/*
 * Original Author: Klusjesman
 *
 * Tested with STK500 + ATMega328P
 * GCC-AVR compiler
 * 
 * Modified by supersjimmie:
 * Code and libraries made compatible with Arduino and ESP8266 
 * Tested with Arduino IDE v1.6.5 and 1.6.9
 * For ESP8266 tested with ESP8266 core for Arduino v 2.1.0 and 2.2.0 Stable
 * (See https://github.com/esp8266/Arduino/ )
 */

/*
CC11xx pins    ESP pins Arduino pins  Description
1 - VCC        VCC      VCC           3v3
2 - GND        GND      GND           Ground

3 - MOSI       13=D7    Pin 11        Data input to CC11xx
4 - SCK        14=D5    Pin 13        Clock pin
5 - MISO/GDO1  12=D6    Pin 12        Data output from CC11xx / serial clock from CC11xx
6 - GDO0       ?        Pin 2?        Serial data to CC11xx
7 - GDO2       ?        Pin  ?        output as a symbol of receiving or sending data
8 - CSN        15=D8    Pin 10        Chip select / (SPI_SS)
*/

#include <SPI.h>
#include <arduino_homekit_server.h>
#include "IthoCC1101.h"
#include "IthoPacket.h"
#include "wifi_info.h"

#define LOG_D(fmt, ...)   printf_P(PSTR(fmt "\n") , ##__VA_ARGS__);

typedef enum Speed 
{
  SpeedIgnore = 0,
  SpeedStandby = 1, 
  SpeedLow = 2, 
  SpeedMedium = 3,
  SpeedHigh  = 4,
  SpeedFull  = 5
};

char *speed_to_string[] = {"ignore", "standby", "low", "medium", "high", "full"};

typedef enum Action 
{
  ActionDeactivate = 1, 
  ActionActivate = 2,
  ActionSpeedStandby = 3,
  ActionSpeedLow = 4,
  ActionSpeedMedium = 5,
  ActionSpeedHigh = 6,
  ActionSpeedFull = 7
};

char *action_to_string[] = {"none", "deactivate", "activate", "standby", "low", "medium", "high", "full"};

IthoCC1101 rf;
IthoPacket packet;
Speed current_speed = SpeedStandby;
Speed last_speed = SpeedStandby;

// access the HomeKit characteristics defined in fan_accessory.c
extern "C" homekit_server_config_t config;
extern "C" homekit_characteristic_t ch_fan_active;
extern "C" homekit_characteristic_t ch_fan_rotation_speed;

static uint32_t next_heap_millis = 0;

void setup(void) {
  Serial.begin(115200);

  wifi_connect(); 
  // homekit_storage_reset(); // to remove the previous HomeKit pairing storage when you first run this new HomeKit example
  homekit_setup();

  delay(500);
  Serial.println("setup begin");
  rf.init();
  Serial.println("setup done");
  sendRegister();
  Serial.println("join command sent");
}

void loop() {
  homekit_loop();
  delay(10);
}

// Called when the fan value is enabled/disabled by iOS Home APP
void ch_fan_active_setter(const homekit_value_t value) {
  LOG_D("Active: %d", value.bool_value);
  
  switch (value.bool_value) {
    case true:  transition(ActionActivate); break;
    case false: transition(ActionDeactivate); break;
  }
}

// Called when the fan rotation speed is changed by iOS Home APP
void ch_fan_rotation_speed_setter(const homekit_value_t value) {
  LOG_D("Rotation speed: %.6f", value.float_value); 

  if (compare_float(value.float_value, 0.0)) {
    transition(ActionSpeedStandby); 
  } else if (compare_float(value.float_value, 1)) {
    transition(ActionSpeedLow); 
  } else if (compare_float(value.float_value, 2)) {
    transition(ActionSpeedMedium);
  } else if (compare_float(value.float_value, 3)) {
    transition(ActionSpeedHigh);
  } else if (compare_float(value.float_value, 4)) {
    transition(ActionSpeedFull);
  } 
}

Speed get_next_speed(Speed current_speed, Action action) {
  switch (current_speed) {
      case SpeedStandby:
        switch (action) {
          case ActionActivate: return last_speed != SpeedStandby ? last_speed : SpeedLow;
          case ActionSpeedLow: return SpeedLow; 
          case ActionSpeedMedium: return SpeedMedium; 
          case ActionSpeedHigh: return SpeedHigh; 
          case ActionSpeedFull: return SpeedFull; 
        }
        break;
        
      case SpeedLow:
        switch (action) {
          case ActionDeactivate: return SpeedStandby; 
          case ActionSpeedMedium: return SpeedMedium; 
          case ActionSpeedHigh: return SpeedHigh; 
          case ActionSpeedFull: return SpeedFull; 
        }
        break;

      case SpeedMedium:
        switch (action) {
          case ActionDeactivate: return SpeedStandby; 
          case ActionSpeedLow: return SpeedLow; 
          case ActionSpeedHigh: return SpeedHigh; 
          case ActionSpeedFull: return SpeedFull;
        }
        break;
        
      case SpeedHigh:
        switch (action) {
          case ActionDeactivate: return SpeedStandby;
          case ActionSpeedLow: return SpeedLow; 
          case ActionSpeedMedium: return SpeedMedium; 
          case ActionSpeedFull: return SpeedFull;
        }
        break;

      case SpeedFull:
        switch (action) {
          case ActionDeactivate: return SpeedStandby;
          case ActionSpeedLow: return SpeedLow; 
          case ActionSpeedMedium: return SpeedMedium; 
          case ActionSpeedHigh: return SpeedHigh; 
        }
        break;        
    }
    
    return SpeedIgnore;
}

void transition(Action action) {
    Speed next_speed = get_next_speed(current_speed, action);

    LOG_D("Transition. Current speed: %s, action: %s, next speed: %s", 
      speed_to_string[current_speed], 
      action_to_string[action], 
      speed_to_string[next_speed]); 
  
    if (next_speed == SpeedIgnore || current_speed == next_speed) {
      LOG_D("Already good. Skipping transition.");
      
      return;
    }

    // Change internal state
    last_speed = current_speed;
    current_speed = next_speed;
    
    // Change Homekit state
    ch_fan_active.value.bool_value = next_speed != SpeedStandby; 
    ch_fan_rotation_speed.value.int_value = next_speed - 1;  

    // Change fan speed state
    if (next_speed == SpeedStandby) {
      sendStandbySpeed();
    } else if (next_speed == SpeedLow) {
      sendLowSpeed();
    } else if (next_speed == SpeedMedium) {
      sendMediumSpeed();
    } else if (next_speed == SpeedHigh) {
      sendHighSpeed();
    } else if (next_speed == SpeedFull) {
      sendFullSpeed();
    }
}

void homekit_setup() {
  ch_fan_active.setter = ch_fan_active_setter;
  ch_fan_rotation_speed.setter = ch_fan_rotation_speed_setter;
  arduino_homekit_setup(&config);

  //report the switch value to HomeKit if it is changed (e.g. by a physical button)
  //homekit_characteristic_notify(&ch_fan_active, ch_fan_active.value);
}

void homekit_loop() {
  arduino_homekit_loop();
  const uint32_t t = millis();
  if (t > next_heap_millis) {
    // show heap info every 30 seconds
    next_heap_millis = t + 30 * 1000;
    LOG_D("Free heap: %d, HomeKit clients: %d", ESP.getFreeHeap(), arduino_homekit_connected_clients_count());
  }
}

//void sendCommand(IthoCommand command) {
//  LOG_D("Sending %d...", command)
//  rf.sendCommand(command);
//  LOG_D("Sending %d done", command)
//}

void sendRegister() {
   Serial.println("sending join...");
   rf.sendCommand(IthoJoin);
   Serial.println("sending join done.");
}

void sendStandbySpeed() {
  Serial.println("sending standby...");
  rf.sendCommand(IthoStandby);
  Serial.println("sending standby done.");
}


void sendLowSpeed() {
   Serial.println("sending low...");
   rf.sendCommand(IthoLow);
   Serial.println("sending low done.");
}

void sendMediumSpeed() {
   Serial.println("sending medium...");
   rf.sendCommand(IthoMedium);
   Serial.println("sending medium done.");
}

void sendHighSpeed() {
  Serial.println("sending high...");
  rf.sendCommand(IthoHigh);
  Serial.println("sending high done.");
}

void sendFullSpeed() {
   Serial.println("sending FullSpeed...");
   rf.sendCommand(IthoFull);
   Serial.println("sending FullSpeed done.");
}

void sendTimer() {
   Serial.println("sending timer...");
   rf.sendCommand(IthoTimer1);
   Serial.println("sending timer done.");
}

 int compare_float(float f1, float f2)
 {
  float precision = 0.00001;
  if (((f1 - precision) < f2) && 
      ((f1 + precision) > f2))
   {
    return 1;
   }
  else
   {
    return 0;
   }
 }
