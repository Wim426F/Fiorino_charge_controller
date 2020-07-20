#include <Arduino.h>
#include <FlexCAN_T4.h>

//FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> can2;
//FlexCAN_T4<CAN2, RX_SIZE_256, TX_SIZE_16> can3;

#define HWSERIAL Serial

void charger_control();
void evse_actuator();

long previousMillis = 0;
unsigned long interval = 10000;

String string_voltage;
String volt;
String volt1;
String string_Vmin;
String string_Vmax;
String string_temp;

float Vmin = 0; // minimum cell voltage
float Vmax = 0; // maximum cell voltage
float temp = 0; // temperature of pack
float Vdif = 0;
float temp_dif = 0;
const float temp_min = 0.0;       // minimum preferred temp before throttling down
const float temp_max = 40.0;      // max preferred temp before throttling down
const float temp_dif_max = 10.0;  // max deviation from temperature limits before charger cut off
const float Vmin_lim = 3.000;     // minimum cell voltage
const float Vmax_lim_low = 4.000; // 70% to 80% charged
const float Vmax_lim_med = 4.100; // 80% to 90% charged
const float Vmax_lim_max = 4.200; // 90% to 100% charged

const byte charger = A0;      // output for pwm signal
const byte charger_limit = 5; // input for 80% charge limit switch
byte charger_pwm = 0;         // pwm variable

boolean plug_locked = false;
const byte plugged_in = 3;     // input for car plugged in
const byte evse_lock_N = A2;   // output for locking, N-channel fet low side
const byte evse_lock_P = A4;   // output for locking fet, P-channel fet high side
const byte evse_unlock_N = A6; // output for unlocking, N-channel fet low side
const byte evse_unlock_P = A8; // output for unlocking, P-channel fet high side

void setup()
{
  Serial.begin(9600);     // serial port to computer
  HWSERIAL.begin(115200); // serial port to the BMS
  delay(1000);

  pinMode(charger, OUTPUT);
  pinMode(plugged_in, INPUT_PULLUP);
  pinMode(charger_limit, INPUT_PULLUP);

  pinMode(evse_lock_N, OUTPUT);
  pinMode(evse_lock_P, OUTPUT);
  pinMode(evse_unlock_N, OUTPUT);
  pinMode(evse_unlock_P, OUTPUT);

  //can2.begin();
  //can2.setBaudRate(250000);
  //can3.begin();
  //can3.setBaudRate(250000);
}

void loop()
{
  Vdif = 0;     // voltage difference between limits for throttling charger
  temp_dif = 0; // temperature difference between limtis for throttling charger
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval)
  {
    HWSERIAL.write("D"); // send the character 'D' every 10 seconds
  }

  while (HWSERIAL.available() > 0 && plugged_in == LOW)
  {
    string_temp = HWSERIAL.readString();
    string_voltage = HWSERIAL.readString();
    //Serial.println(string_voltage);
    if (string_temp.indexOf("00-00-d2-7f 049") > -1)
    {
      string_temp.remove(0, 30);
      if (string_temp.startsWith('-')) // check if temperature is below 0°C
      {
        temp = string_temp.toFloat();
      }
      else
      {
        string_temp.remove(0, 1);
        temp = string_temp.toFloat();
      }
    }

    if (string_voltage.indexOf("Vmin") > -1 && string_voltage.startsWith("7")) // find the string Vmin in the buffer
    {
      string_voltage.remove(0, 13);
      volt = string_voltage.substring(0, 55); // extract char 13 to 51 from string
      volt1 = string_voltage.substring(0, 55);
      if (volt1.indexOf("Vmax") > -1 && Vmax > 0)
      {
        volt1.remove(0, 33);
        string_Vmax = volt1.substring(0, 5);
      }
      else
      {
        volt1.remove(0, 16);
        string_Vmin = volt1.substring(0, 5);
      }
      string_Vmin = volt.substring(0, 5); // extract char 0 to 5 from substring for minimum cell voltage
      Vmin = string_Vmin.toFloat();       // convert string to float
      Vmax = string_Vmax.toFloat();
      previousMillis = currentMillis;
    }
  }
  Serial.println("Vmin: " + string_Vmin + "     Vmax: " + string_Vmin);
  //Serial.println(string_temp);
  charger_control();
  evse_actuator();
}

void charger_control()
{
  if (Vmin < Vmin_lim)
  {
    Vdif = 229 - (Vmin_lim - Vmin) * 200;
    if (Vdif < 26)
    {
      Vdif = 26;
    }
  }

  if (Vmin > Vmin_lim && Vmax < Vmax_lim_low) // if cell voltage is in between high and low limits, charger max current
  {
    Vdif = 229;
  }

  if (Vmax >= Vmax_lim_low && Vmax <= Vmax_lim_med && digitalRead(charger_limit) == HIGH) // if limit switch is set to first limit start throttling down from low to med
  {
    Vdif = (Vmax_lim_med - Vmax) * 2040 + 25; // PWM range is from 10% to 90% so from 255 points the range is 204, starting from 26(10%)
  }

  if (Vmax > Vmax_lim_low && Vmax < Vmax_lim_max && digitalRead(charger_limit) == LOW) // if max cell voltage is between second limit, keep throttling down if limit switch allows
  {
    Vdif = (Vmax_lim_max - Vmax) * 1020 + 25;
  }

  if ((Vmax > Vmax_lim_max && digitalRead(charger_limit) == LOW) || (Vmax > Vmax_lim_med && digitalRead(charger_limit) == HIGH))
  {
    Vdif = 0;
  }

  if (temp <= temp_min && temp_min - temp < temp_dif_max) // check if temperature is outside of preferred range but still within max deviation
  {
    temp_dif = temp_min - temp;
    Vdif = Vdif - (temp_dif * 10); // subtract the temp difference times 10 from the Vdif for throttling down
  }

  if (temp >= temp_max && temp - temp_max < temp_dif_max) // check if temperature is outside of preferred range but still within max deviation
  {
    temp_dif = temp - temp_max;
    Vdif = Vdif - (temp_dif * 10);
  }

  charger_pwm = Vdif;
  analogWrite(charger, charger_pwm);
}

void evse_actuator()
{
  if (digitalRead(plugged_in) == LOW && plug_locked == false) // if the evse is plugged in and not latched
  {
    analogWrite(evse_unlock_P, 255); // switch P fet unlock off
    analogWrite(evse_unlock_N, 0);   // switch N fet unlock off
    delayNanoseconds(150);           // wait for fet delay & fall time
    analogWrite(evse_lock_P, 0);     // switch P fet lock on
    analogWrite(evse_lock_N, 255);   // switch N fet lock on
    delayNanoseconds(150);
    plug_locked = true;
  }

  if (digitalRead(plugged_in) == HIGH && plug_locked == true) // if the evse is stopped
  {
    analogWrite(evse_lock_P, 255);   // turn P fet lock off
    analogWrite(evse_lock_N, 0);     // turn N fet lock off
    delayNanoseconds(150);           // wait for fet delay & fall time
    analogWrite(evse_unlock_P, 0);   // turn P fet unlock on
    analogWrite(evse_unlock_N, 255); // turn N fet unlock on
    plug_locked = false;
  }
}