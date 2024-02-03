#include <sys/time.h>
#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#endif
#ifdef ARDUINO_ARCH_ESP8266
#include <ESP8266WiFi.h>
#endif
#include <errno.h>
#include <WiFiUdp.h>
#include "time.h"
#include "SerialCommands.h"
#include "EEPROM.h"
#include "sntp.h"

#define LED           2
#define QRD1114_PIN   0

#ifndef VERBOSE
#define VERBOSE
#endif
#ifndef DEBUG
#define DEBUG
#endif

/* NTP server to use, can be configured later on via AT commands */
#ifndef DEFAULT_NTP_SERVER
#define DEFAULT_NTP_SERVER "at.pool.ntp.org"
#endif

/* ESP yield */
#if ARDUINO_ARCH_ESP8266 || ARDUINO_ARCH_ESP32
 #define doYIELD yield();
#else
 #define doYIELD
#endif

/* our AT commands over UART to config OTP's and WiFi */
char atscbu[128] = {""};
SerialCommands ATSc(&Serial, atscbu, sizeof(atscbu), "\r\n", "\r\n");

#define CFGVERSION 0x01 // switch between 0x01/0x02 to reinit the config struct change
#define CFGINIT    0x72 // at boot init check flag
#define CFG_EEPROM 0x00 

/* main config */
struct wm_cfg {
  uint8_t initialized  = 0;
  uint8_t version      = 0;
  uint8_t do_debug     = 0;
  uint16_t main_loop_delay = 0;
  uint8_t do_verbose   = 0;
  uint8_t do_log       = 0;
  uint8_t persist_counter_reboot  = 0;
  unsigned long persist_interval  = 0;
  double rate_adjust   = 0;
  short udp_port       = 0;
  char udp_host_ip[16] = {0};
  unsigned int log_interval_1s = 0;
  unsigned int log_interval_5m = 0;
  char wifi_ssid[32]   = {0};   // max 31 + 1
  char wifi_pass[64]   = {0};   // nax 63 + 1
  char ntp_host[64]    = {0};   // max hostname + 1
};
typedef wm_cfg wm_cfg_t;
wm_cfg_t cfg = {0};

/* our main counter values in memory */
uint32_t last_cnt = 0;
struct cnt_state {
  uint32_t current_counter = 0;
};
typedef cnt_state cnt_state_t;
cnt_state_t counter;
double rate = 0.0;

/* state flags */
uint8_t ntp_is_synced      = 1;
uint8_t logged_wifi_status = 0;
unsigned int last_log_value_1s = 0;
unsigned int last_log_value_5m = 0;
unsigned int last_saved_v  = 0;
unsigned long last_chg     = 0;
unsigned int v = 0, last_v = 0;
unsigned long last_log_time_1s = 0;
unsigned long last_log_time_5m = 0;
unsigned long last_saved_t = 0;
unsigned long last_wifi_check = 0;

#define UNIT               0.5
#define LAST_STATE_UNKNOWN 2
#define LAST_STATE_UP      1
#define LAST_STATE_DOWN    0
uint8_t last_was_up = LAST_STATE_UNKNOWN;

// for output to a remote server
#define OUTBUFFER_SIZE  128
WiFiUDP udp;
IPAddress udp_tgt;
uint8_t valid_udp_host = 0;
char outbuffer[OUTBUFFER_SIZE] = {0};
int h_strl = 0;

void(* resetFunc)(void) = 0;

char* at_cmd_check(const char *cmd, const char *at_cmd, unsigned short at_len){
  unsigned short l = strlen(cmd); /* AT+<cmd>=, or AT, or AT+<cmd>? */
  if(at_len >= l && strncmp(cmd, at_cmd, l) == 0){
    if(*(cmd+l-1) == '='){
      return (char *)at_cmd+l;
    } else {
      return (char *)at_cmd;
    }
  }
  return NULL;
}

void at_cmd_handler(SerialCommands* s, const char* atcmdline){
  unsigned int cmd_len = strlen(atcmdline);
  char *p = NULL;
  #ifdef AT_DEBUG
  Serial.print(F("AT: ["));
  Serial.print(atcmdline);
  Serial.print(F("], size: "));
  Serial.println(cmd_len);
  #endif
  if(cmd_len == 2 && (p = at_cmd_check("AT", atcmdline, cmd_len))){
  } else if(p = at_cmd_check("AT+WIFI_SSID=", atcmdline, cmd_len)){
    size_t sz = (atcmdline+cmd_len)-p+1;
    if(sz > 31){
      s->GetSerial()->println(F("WiFI SSID max 31 chars"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    strncpy((char *)&cfg.wifi_ssid, p, sz);
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
    WiFi.disconnect();
    setup_wifi();
    setup_udp();
    configTime(0, 0, (char *)&cfg.ntp_host);
  } else if(p = at_cmd_check("AT+WIFI_SSID?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.wifi_ssid);
    return;
  } else if(p = at_cmd_check("AT+WIFI_PASS=", atcmdline, cmd_len)){
    size_t sz = (atcmdline+cmd_len)-p+1;
    if(sz > 63){
      s->GetSerial()->println(F("WiFI password max 63 chars"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    strncpy((char *)&cfg.wifi_pass, p, sz);
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
    WiFi.disconnect();
    setup_wifi();
    setup_udp();
    configTime(0, 0, (char *)&cfg.ntp_host);
  #ifdef DEBUG
  } else if(p = at_cmd_check("AT+WIFI_PASS?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.wifi_pass);
  #endif
  } else if(p = at_cmd_check("AT+WIFI_STATUS?", atcmdline, cmd_len)){
    uint8_t wifi_stat = WiFi.status();
    switch(wifi_stat) {
        case WL_CONNECTED:
          s->GetSerial()->println(F("connected"));
          break;
        case WL_CONNECT_FAILED:
          s->GetSerial()->println(F("failed"));
          break;
        case WL_CONNECTION_LOST:
          s->GetSerial()->println(F("connection lost"));
          break;
        case WL_DISCONNECTED:
          s->GetSerial()->println(F("disconnected"));
          break;
        case WL_IDLE_STATUS:
          s->GetSerial()->println(F("idle"));
          break;
        case WL_NO_SSID_AVAIL:
          s->GetSerial()->println(F("no SSID configured"));
          break;
        default:
          s->GetSerial()->println(wifi_stat);
    }
    return;
  } else if(p = at_cmd_check("AT+NTP_HOST=", atcmdline, cmd_len)){
    size_t sz = (atcmdline+cmd_len)-p+1;
    if(sz > 63){
      s->GetSerial()->println(F("NTP hostname max 63 chars"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    strncpy((char *)&cfg.ntp_host, p, sz);
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
    setup_wifi();
    configTime(0, 0, (char *)&cfg.ntp_host);
  } else if(p = at_cmd_check("AT+NTP_HOST?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.ntp_host);
    return;
  } else if(p = at_cmd_check("AT+NTP_STATUS?", atcmdline, cmd_len)){
    if(ntp_is_synced)
      s->GetSerial()->println(F("ntp synced"));
    else
      s->GetSerial()->println(F("not ntp synced"));
    return;
  } else if(p = at_cmd_check("AT+CNT=", atcmdline, cmd_len)){
    errno = 0;
    double new_d = strtod(p, NULL);
    if(errno != 0){
      s->GetSerial()->println(F("invalid double"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    unsigned int new_c = (unsigned int)floor(new_d*(1/UNIT));
    if(new_c != counter.current_counter){
      #ifdef VERBOSE
      if(cfg.do_verbose){
        Serial.print(F("set new counter of 0.5L to: "));
        Serial.println(new_c);
      }
      #endif
      counter.current_counter = new_c;
      // disable next rate calculation, it'll be pointless
      last_log_time_1s  = 0;
      last_log_value_1s = 0;
      last_log_time_5m  = 0;
      last_log_value_5m = 0;
    }
  } else if(p = at_cmd_check("AT+CNT?", atcmdline, cmd_len)){
    s->GetSerial()->println(((double)counter.current_counter)*UNIT);
  #ifdef DEBUG
  } else if(p = at_cmd_check("AT+DEBUG=1", atcmdline, cmd_len)){
    cfg.do_debug = 1;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+DEBUG=0", atcmdline, cmd_len)){
    cfg.do_debug = 0;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+DEBUG?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.do_debug);
  #endif
  #ifdef VERBOSE
  } else if(p = at_cmd_check("AT+VERBOSE=1", atcmdline, cmd_len)){
    cfg.do_verbose = 1;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+VERBOSE=0", atcmdline, cmd_len)){
    cfg.do_verbose = 0;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+VERBOSE?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.do_verbose);
  #endif
  } else if(p = at_cmd_check("AT+LOG_UART=1", atcmdline, cmd_len)){
    cfg.do_log = 1;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+LOG_UART=0", atcmdline, cmd_len)){
    cfg.do_log = 0;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+LOG_UART?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.do_log);
  } else if(p = at_cmd_check("AT+COUNTER_SAVE?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.persist_counter_reboot);
  } else if(p = at_cmd_check("AT+COUNTER_SAVE=1", atcmdline, cmd_len)){
    cfg.persist_counter_reboot = 1;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+COUNTER_SAVE=0", atcmdline, cmd_len)){
    cfg.persist_counter_reboot = 0;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+COUNTER_SAVE_INTERVAL?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.persist_interval);
  } else if(p = at_cmd_check("AT+COUNTER_SAVE_INTERVAL=", atcmdline, cmd_len)){
    errno = 0;
    unsigned int l_int = (double)strtoul(p, NULL, 10);
    if(errno != 0){
      s->GetSerial()->println(F("invalid integer"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    if(l_int != cfg.persist_interval){
      cfg.persist_interval = l_int;
      EEPROM.put(CFG_EEPROM, cfg);
      EEPROM.commit();
    }
  } else if(p = at_cmd_check("AT+RATE_FACTOR?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.rate_adjust);
  } else if(p = at_cmd_check("AT+RATE_FACTOR=", atcmdline, cmd_len)){
    errno = 0;
    double r = (double)strtod(p, NULL);
    if(errno != 0){
      s->GetSerial()->println(F("invalid double"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    if(r != cfg.rate_adjust){
      cfg.rate_adjust = r;
      EEPROM.put(CFG_EEPROM, cfg);
      EEPROM.commit();
    }
  } else if(p = at_cmd_check("AT+LOG1_INTERVAL?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.log_interval_1s);
  } else if(p = at_cmd_check("AT+LOG1_INTERVAL=", atcmdline, cmd_len)){
    errno = 0;
    unsigned int l_int = (double)strtoul(p, NULL, 10);
    if(errno != 0){
      s->GetSerial()->println(F("invalid integer"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    if(l_int != cfg.log_interval_1s){
      cfg.log_interval_1s = l_int;
      EEPROM.put(CFG_EEPROM, cfg);
      EEPROM.commit();
    }
  } else if(p = at_cmd_check("AT+LOG2_INTERVAL?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.log_interval_5m);
  } else if(p = at_cmd_check("AT+LOG2_INTERVAL=", atcmdline, cmd_len)){
    errno = 0;
    unsigned int l_int = (double)strtoul(p, NULL, 10);
    if(errno != 0){
      s->GetSerial()->println(F("invalid integer"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    if(l_int != cfg.log_interval_5m){
      cfg.log_interval_5m = l_int;
      EEPROM.put(CFG_EEPROM, cfg);
      EEPROM.commit();
    }
  } else if(p = at_cmd_check("AT+UDP_PORT?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.udp_port);
  } else if(p = at_cmd_check("AT+UDP_PORT=", atcmdline, cmd_len)){
    cfg.udp_port = strtol(p, NULL, 10);
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
    setup_udp();
  } else if(p = at_cmd_check("AT+UDP_HOST_IP?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.udp_host_ip);
  } else if(p = at_cmd_check("AT+UDP_HOST_IP=", atcmdline, cmd_len)){
    IPAddress tst;
    if(!tst.fromString(p)){
      s->GetSerial()->println(F("invalid udp host IP"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    strcpy(cfg.udp_host_ip, p);
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
    setup_udp();
  } else if(p = at_cmd_check("AT+LOOP_DELAY=", atcmdline, cmd_len)){
    errno = 0;
    unsigned int new_c = strtoul(p, NULL, 10);
    if(errno != 0){
      s->GetSerial()->println(F("invalid integer"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    if(new_c != cfg.main_loop_delay){
      cfg.main_loop_delay = new_c;
      EEPROM.put(CFG_EEPROM, cfg);
      EEPROM.commit();
    }
  } else if(p = at_cmd_check("AT+LOOP_DELAY?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.main_loop_delay);
  } else if(p = at_cmd_check("AT+ERASE", atcmdline, cmd_len)){
    memset(&cfg, 0, sizeof(cfg));
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
    memset(&counter, 0, sizeof(counter));
    EEPROM.put(CFG_EEPROM+sizeof(counter), counter);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+RESET", atcmdline, cmd_len)){
    s->GetSerial()->println(F("OK"));
    resetFunc();
    return;
  } else {
    s->GetSerial()->println(F("ERROR"));
    return;
  }
  s->GetSerial()->println(F("OK"));
  return;
}

void setup() {
  // Serial setup, init at 115200 8N1
  Serial.begin(115200, SERIAL_8N1);

  // EEPROM read
  EEPROM.begin(sizeof(cfg)+sizeof(counter));
  EEPROM.get(CFG_EEPROM, cfg);
  // was (or needs) initialized?
  if(cfg.initialized != CFGINIT || cfg.version != CFGVERSION){
    // clear
    memset(&cfg, 0, sizeof(cfg));
    // reinit
    cfg.initialized     = CFGINIT;
    cfg.version         = CFGVERSION;
    cfg.do_log          = 1;
    cfg.main_loop_delay = 100;
    cfg.log_interval_1s = 1000;
    cfg.log_interval_5m = 30000;
    cfg.rate_adjust     = 1.0;
    cfg.persist_counter_reboot = 1;
    cfg.persist_interval       = 3600000;
    strcpy((char *)&cfg.ntp_host, (char *)DEFAULT_NTP_SERVER);
    // write
    EEPROM.put(CFG_EEPROM, cfg);
    // write empty counter
    memset(&counter, 0, sizeof(counter));
    EEPROM.put(CFG_EEPROM+sizeof(cfg), counter);
    EEPROM.commit();
  }

  // read in counter state
  EEPROM.get(CFG_EEPROM+sizeof(cfg), counter);

  // tgt UDP setup
  setup_udp();

  #ifdef VERBOSE
  if(cfg.do_verbose){
    Serial.print(F("eeprom size: "));
    Serial.println(EEPROM.length());
  }
  #endif

  #ifdef VERBOSE
  if(cfg.do_verbose){
    if(strlen(cfg.wifi_ssid) && strlen(cfg.wifi_pass)){
      Serial.print(F("will connect via WiFi to "));
      Serial.println(cfg.wifi_ssid);
    } else {
      Serial.print(F("will not connect via WiFi, no ssid/pass"));
    }
  }
  #endif

  // Setup AT command handler
  ATSc.SetDefaultHandler(&at_cmd_handler);

  // see http://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
  // for OTP tokens, this ALWAYS have to be "UTC"
  setenv("TZ", "UTC", 1);
  tzset();

  // setup WiFi with ssid/pass from EEPROM if set
  setup_wifi();

  #ifdef VERBOSE
  if(cfg.do_verbose){
    if(strlen(cfg.ntp_host) && strlen(cfg.wifi_ssid) && strlen(cfg.wifi_pass)){
      Serial.print(F("will sync with ntp, wifi ssid/pass ok: "));
      Serial.println(cfg.ntp_host);
    }
  }
  #endif
  // setup NTP sync to RTC
  configTime(0, 0, (char *)&cfg.ntp_host);
  // new time - set by NTP
  time_t t;
  struct tm gm_new_tm;
  time(&t);
  localtime_r(&t, &gm_new_tm);
  #ifdef VERBOSE
  if(cfg.do_verbose){
    Serial.print(F("now: "));
    Serial.println(t);
    char d_outstr[100];
    strftime(d_outstr, 100, "current time: %A, %B %d %Y %H:%M:%S (%s)", &gm_new_tm);
    Serial.println(d_outstr);
  }
  #endif

  // led to show status
  pinMode(LED, OUTPUT);

  // QRD1114 PIN set as INPUT
  pinMode(QRD1114_PIN, INPUT);
}
 
void loop() {
  // any new AT command? on USB uart
  ATSc.ReadSerial();

  delay(cfg.main_loop_delay);

  // check PIN value
  v = digitalRead(QRD1114_PIN);
  if(v != last_v){
    #ifdef DEBUG
    if(cfg.do_debug){
      Serial.print(F("V: "));
      Serial.print(v);
      Serial.print(F(", LAST_V: "));
      Serial.println(last_v);
    }
    #endif

    // up: +1/2 counter value
    if(v > last_v && (last_was_up == LAST_STATE_UP || last_was_up == LAST_STATE_UNKNOWN)){
      last_was_up = LAST_STATE_DOWN;
      counter.current_counter++;
    }
    // down: +1/2 counter value
    if(v < last_v && (last_was_up == LAST_STATE_DOWN || last_was_up == LAST_STATE_UNKNOWN)){
      last_was_up = LAST_STATE_UP;
      counter.current_counter++;
    }
    last_v = v;
  }

  // led status change based on counter change?
  if(last_cnt != counter.current_counter){
    digitalWrite(LED, LOW);
    digitalWrite(LED, HIGH);
    last_cnt = counter.current_counter;
    last_chg = millis();
  } else {
    if(millis() - last_chg > 50)
      digitalWrite(LED, LOW);
  }

  // last log check TIMER/RATE 1
  if(millis() - last_log_time_1s > cfg.log_interval_1s){
    if(last_log_time_1s == 0){
      last_log_value_1s = counter.current_counter;
      last_log_time_1s  = millis();
    } else {
      if(last_log_value_1s != 0){
        // 1000* as this is in millis()
        // rate_adjust is 1, but 1/2 as 1 tick is 1/2L
        rate = 1000*cfg.rate_adjust*UNIT*(double)(counter.current_counter-last_log_value_1s)/(double)(millis()-last_log_time_1s);
      } else {
        rate = 0.0;
      }
      last_log_value_1s = counter.current_counter;
      last_log_time_1s  = millis();

      memset((char*)&outbuffer, 0, OUTBUFFER_SIZE);
      h_strl = snprintf((char *)&outbuffer, OUTBUFFER_SIZE, "C,%.1f\r\nR1,%.8f\r\n", (double)(last_log_value_1s)*UNIT, rate);

      // output over UART?
      if(cfg.do_log)
        Serial.print(outbuffer);

      // output over UDP?
      if(valid_udp_host){
        udp.beginPacket(udp_tgt, cfg.udp_port);
        udp.write(outbuffer, h_strl);
        udp.endPacket();
      }
    }
  }

  // last log check TIMER/RATE 2
  if(millis() - last_log_time_5m > cfg.log_interval_5m){
    if(last_log_value_5m == 0){
      last_log_value_5m   = counter.current_counter;
      last_log_time_5m    = millis();
    } else {
      // see 1s
      if(last_log_value_5m != 0){
        rate = 1000*cfg.rate_adjust*UNIT*(double)(counter.current_counter-last_log_value_5m)/(double)(millis()-last_log_time_5m);
      } else {
        rate = 0.0;
      }
      last_log_value_5m   = counter.current_counter;
      last_log_time_5m    = millis();

      memset((char*)&outbuffer, 0, OUTBUFFER_SIZE);
      h_strl = snprintf((char *)&outbuffer, OUTBUFFER_SIZE, "R2,%.8f\r\n", rate);

      // output over UART?
      if(cfg.do_log)
        Serial.print(outbuffer);

      // output over UDP?
      if(valid_udp_host){
        udp.beginPacket(udp_tgt, cfg.udp_port);
        udp.write(outbuffer, h_strl);
        udp.endPacket();
      }
    }
  }

  // save to EEPROM?
  if(cfg.persist_counter_reboot){
    if(millis() - last_saved_t > cfg.persist_interval){
      if(last_saved_v != counter.current_counter){
        #ifdef DEBUG
        if(cfg.do_debug)
          Serial.println(F("EEPROM save"));
        #endif
        last_saved_v = counter.current_counter;
        EEPROM.put(CFG_EEPROM+sizeof(cfg), counter);
        EEPROM.commit();
      }
      last_saved_t = millis();
    }
  }

  // just wifi check
  if(millis() - last_wifi_check > 500){
    if(WiFi.status() == WL_CONNECTED){
      if(!logged_wifi_status){
        #ifdef VERBOSE
        if(cfg.do_verbose){
          Serial.print(F("WiFi connected: "));
          Serial.println(WiFi.localIP());
        }
        #endif
        logged_wifi_status = 1;
      }
      if(!valid_udp_host)
        setup_udp();
    } else {
      valid_udp_host = 0;
    }
  }
}

void setup_wifi(){
  // are we connecting to WiFi?
  if(strlen(cfg.wifi_ssid) == 0 || strlen(cfg.wifi_pass) == 0)
    return;
  if(WiFi.status() == WL_CONNECTED)
    return;

  // connect to Wi-Fi
  #ifdef VERBOSE
  if(cfg.do_verbose){
    Serial.print(F("Connecting to "));
    Serial.println(cfg.wifi_ssid);
  }
  #endif
  WiFi.persistent(false);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
}

void setup_udp(){
  if(udp_tgt.fromString(cfg.udp_host_ip) && cfg.udp_port > 0){
    valid_udp_host = 1;
    #ifdef VERBOSE
    if(cfg.do_verbose){
      Serial.print(F("send counters to "));
      Serial.print(cfg.udp_host_ip);
      Serial.print(F(":"));
      Serial.println(cfg.udp_port);
    }
    #endif
  } else {
    valid_udp_host = 0;
    #ifdef VERBOSE
    if(cfg.do_verbose){
      Serial.print(F("udp target host/port is not valid"));
      Serial.println(cfg.udp_host_ip);
    }
    #endif
  }
}
