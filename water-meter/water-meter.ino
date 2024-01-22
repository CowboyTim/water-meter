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

#define CFGVERSION 0x01
#define CFGINIT    0x72

/* main config */
struct wm_cfg {
  byte initialized   = 0;
  byte version       = 0;
  char wifi_ssid[32] = {0};   // max 31 + 1
  char wifi_pass[64] = {0};   // nax 63 + 1
  char ntp_host[64]  = {0};   // max hostname + 1
  unsigned int cnt   = 0;
  unsigned short wifi_timeout = 0;
  uint8_t do_debug   = 0;
  uint8_t do_verbose = 0;
  uint8_t do_log     = 0;
  double rate_change = 0;
  int udp_port       = 0;
  char udp_host_ip[16] = {0};
};
typedef wm_cfg wm_cfg_t;
wm_cfg_t cfg = {0};

/* ntp state */
uint8_t ntp_is_synced      = 1;
uint8_t logged_wifi_status = 0;
unsigned int last_cnt      = 0;
unsigned int last_log_value= 0;
unsigned long last_chg     = 0;
unsigned int v = 0, last_v = 0;
unsigned long last_log_time= 0;
unsigned long nw           = 0;
unsigned long last_wifi_check = 0;
double rate = 0.0;

// for output to a remote server
WiFiUDP udp;
IPAddress udp_tgt;
uint8_t valid_udp_host = 0;

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
    strncpy((char *)&cfg.wifi_ssid, p, (atcmdline+cmd_len)-p+1);
    EEPROM.put(0, cfg);
    EEPROM.commit();
    WiFi.disconnect();
    setup_wifi();
    setup_udp();
    configTime(0, 0, (char *)&cfg.ntp_host);
  } else if(p = at_cmd_check("AT+WIFI_SSID?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.wifi_ssid);
    return;
  } else if(p = at_cmd_check("AT+WIFI_PASS=", atcmdline, cmd_len)){
    strncpy((char *)&cfg.wifi_pass, p, (atcmdline+cmd_len)-p+1);
    EEPROM.put(0, cfg);
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
    strncpy((char *)&cfg.ntp_host, p, (atcmdline+cmd_len)-p+1);
    EEPROM.put(0, cfg);
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
    cfg.cnt = strtol(p, NULL, 10);
  } else if(p = at_cmd_check("AT+CNT?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.cnt);
  #ifdef DEBUG
  } else if(p = at_cmd_check("AT+DEBUG=1", atcmdline, cmd_len)){
    cfg.do_debug = 1;
    EEPROM.put(0, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+DEBUG=0", atcmdline, cmd_len)){
    cfg.do_debug = 0;
    EEPROM.put(0, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+DEBUG?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.do_debug);
  #endif
  #ifdef VERBOSE
  } else if(p = at_cmd_check("AT+VERBOSE=1", atcmdline, cmd_len)){
    cfg.do_verbose = 1;
    EEPROM.put(0, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+VERBOSE=0", atcmdline, cmd_len)){
    cfg.do_verbose = 0;
    EEPROM.put(0, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+VERBOSE?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.do_verbose);
  #endif
  } else if(p = at_cmd_check("AT+LOG_CNT=1", atcmdline, cmd_len)){
    cfg.do_log = 1;
    EEPROM.put(0, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+LOG_CNT=0", atcmdline, cmd_len)){
    cfg.do_log = 0;
    EEPROM.put(0, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+LOG_CNT?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.do_log);
  } else if(p = at_cmd_check("AT+RATE_FACTOR=", atcmdline, cmd_len)){
    errno = 0;
    double r = (double)strtod(p, NULL);
    if(errno != 0){
      s->GetSerial()->println(F("invalid double"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    if(r != cfg.rate_change){
      cfg.rate_change = r;
      EEPROM.put(0, cfg);
      EEPROM.commit();
    }
  } else if(p = at_cmd_check("AT+UDP_PORT?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.udp_port);
  } else if(p = at_cmd_check("AT+UDP_HOST_IP?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.udp_host_ip);
  } else if(p = at_cmd_check("AT+UDP_PORT=", atcmdline, cmd_len)){
    cfg.udp_port = strtol(p, NULL, 10);
    EEPROM.put(0, cfg);
    EEPROM.commit();
    setup_udp();
  } else if(p = at_cmd_check("AT+UDP_HOST_IP=", atcmdline, cmd_len)){
    IPAddress tst;
    if(!tst.fromString(p)){
      s->GetSerial()->println(F("invalid udp host IP"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    strcpy(cfg.udp_host_ip, p);
    EEPROM.put(0, cfg);
    EEPROM.commit();
    setup_udp();
  } else if(p = at_cmd_check("AT+RATE_FACTOR?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.rate_change);
  } else if(p = at_cmd_check("AT+ERASE", atcmdline, cmd_len)){
    memset(&cfg, 0, sizeof(cfg));
    EEPROM.put(0, cfg);
    EEPROM.commit();
  } else {
    s->GetSerial()->println(F("ERROR"));
    return;
  }
  s->GetSerial()->println(F("OK"));
  return;
}

void setup() {
  // Serial setup/debug
  Serial.begin(115200);

  // EEPROM read
  EEPROM.begin(sizeof(cfg));
  EEPROM.get(0, cfg);
  // was (or needs) initialized?
  if(cfg.initialized != CFGINIT || cfg.version != CFGVERSION){
    // clear
    memset(&cfg, 0, sizeof(cfg));
    // reinit
    cfg.initialized = CFGINIT;
    cfg.version     = CFGVERSION;
    cfg.do_log      = 1;
    cfg.rate_change = 0.1;
    strcpy((char *)&cfg.ntp_host, (char *)DEFAULT_NTP_SERVER);
    // write
    EEPROM.put(0, cfg);
    EEPROM.commit();
  }

  // tgt UDP setup
  setup_udp();

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
    if(v < last_v)
      cfg.cnt++;
    last_v = v;
  }

  // led status change based on counter change?
  if(last_cnt != cfg.cnt){
    digitalWrite(LED, HIGH);
    digitalWrite(LED, LOW);
    last_cnt = cfg.cnt;
    last_chg = millis();
  } else {
    if(millis() - last_chg > 10)
      digitalWrite(LED, HIGH);
  }

  // last log check
  nw = millis();
  if(nw - 1000 > last_log_time){
    if(last_log_value != 0 && last_log_time != 0){
      rate = cfg.rate_change*(double)(last_cnt-last_log_value)/(double)(nw-last_log_time);
    } else {
      rate = 0.0;
    }
    last_log_value = last_cnt;
    last_log_time  = nw;

    char b[50] = {0};
    int h_strl = snprintf((char *)&b, 50, "C,%d\r\nR,%0.04f\r\n", last_cnt, rate);

    // output over UART?
    if(cfg.do_log)
      Serial.print(b);

    // output over UDP?
    if(valid_udp_host){
      udp.beginPacket(udp_tgt, cfg.udp_port);
      udp.write(b, h_strl);
      udp.endPacket();
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
