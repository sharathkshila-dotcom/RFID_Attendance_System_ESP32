#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

const char *WIFI_SSID = "VHK";
const char *WIFI_PASSWORD = "varunhkk1";
const char *GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbx_K3G77hTd3O4iCj1Ci_GkL1PIJngPnl_Mbz-qy1q6Np0PtzVtN2-O6rRgbPr9NYUM/exec";

// ---------- RFID pins ----------
#define RST_PIN 22
#define SS_PIN 5
#define BUZZER 15

// ---------- OLED pins/config ----------
// NOTE: I2C is moved to custom pins to avoid conflicts with:
//   - RST_PIN (22), which is ESP32's default I2C SCL pin
//   - GPIO 23, which is the default SPI MOSI pin used by the RFID module
// SDA = 21 (free), SCL = 2 (free)
#define OLED_SDA 21
#define OLED_SCL 2
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

MFRC522 mfrc522(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key;
MFRC522::StatusCode status;

Preferences prefs;
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};

byte rowPins[ROWS] = {4, 13, 14, 25};
byte colPins[COLS] = {26, 27, 32, 33};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

int blockNum = 4;
byte readBlockData[18];
String Master[1] = {"63:56:4B:13"};
String uid;

const int MAX_NAMES = 10;
String nameList[MAX_NAMES];
int noOfNames = 0;

void readMode();
void writeMode();
void deleteMode();
void processAccessCheck();
void ReadDataFromBlock(int blockNum, byte readBlockData[]);
void WriteDataToBlock(int blockNum, byte blockData[]);
void checkGlobalReset();
void addNameToList(String newName);
bool removeNameFromList(String targetName);
void loadNamesFromFlash();
void saveNamesToFlash();
void connectWiFi();
void sendToGoogleSheet(String name, String uidStr, String accessStatus);
String urlEncode(String str);

// ---------- OLED helper functions ----------
void oledMessage(String line1, String line2 = "", String line3 = "");
void oledIdleScreen();
void oledSplashScreen();

void setup()
{
  Serial.begin(9600);
  SPI.begin();
  mfrc522.PCD_Init();

  pinMode(BUZZER, OUTPUT);

  // ---- OLED init ----
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
  {
    // The OLED is the only visual feedback the user gets (WiFi status,
    // access results, write/delete prompts), so a failed init halts
    // the system rather than running blind.
    Serial.println("OLED init failed! Check wiring/address.");
    while (true)
      ;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Initializing...");
  display.display();

  for (byte i = 0; i < 6; i++)
    key.keyByte[i] = 0xFF;

  oledSplashScreen();
  delay(1200);

  connectWiFi();
  loadNamesFromFlash();

  Serial.println("--- System Ready ---");
  Serial.print("Loaded ");
  Serial.print(noOfNames);
  Serial.println(" authorized name(s) from flash.");
  Serial.println("Authorized list:");
  for (int i = 0; i < noOfNames; i++)
  {
    Serial.print("  ");
    Serial.print(i);
    Serial.print(": ");
    Serial.println(nameList[i]);
  }

  oledMessage("System Ready", String(noOfNames) + " users loaded",
              WiFi.status() == WL_CONNECTED ? "WiFi: Connected" : "WiFi: Offline");
  delay(1200);
  oledIdleScreen();
}

void loop()
{
  checkGlobalReset();

  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial())
    return;

  uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++)
  {
    if (mfrc522.uid.uidByte[i] < 0x10)
      uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
    if (i < mfrc522.uid.size - 1)
      uid += ":";
  }

  uid.toUpperCase();
  Master[0].toUpperCase();

  Serial.print("Card UID: ");
  Serial.println(uid);

  if (uid == Master[0])
  {
    // Master card behaves like a normal card by default (runs the same
    // access check). It only branches into the admin menu if a menu key
    // (A/B/#/D) is actually pressed within the listening window.
    Serial.println("Master Card Detected! Scanning normally...");
    Serial.println("Press A (Delete) / B (Write) / # (Read again) / D (Reset) to switch mode...");

    oledMessage("Master Card", "A=Del B=Write", "#=Read D=Reset");

    unsigned long startTime = millis();
    bool modeSelected = false;

    while (millis() - startTime < 5000)
    {
      char key_pressed = keypad.getKey();

      if (key_pressed == 'D')
      {
        Serial.println("[!] Resetting System...");
        oledMessage("Resetting...");
        delay(300);
        ESP.restart();
      }

      if (key_pressed == '#')
      {
        Serial.println("--- Mode: READ ---");
        delay(300);
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();
        readMode();
        modeSelected = true;
        break;
      }

      if (key_pressed == 'B')
      {
        Serial.println("--- Mode: WRITE ---");
        delay(300);
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();
        writeMode();
        modeSelected = true;
        break;
      }

      if (key_pressed == 'A')
      {
        Serial.println("--- Mode: DELETE ---");
        delay(300);
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();
        deleteMode();
        modeSelected = true;
        break;
      }
    }

    if (!modeSelected)
    {
      // No key was pressed in time -> treat this tap exactly like a
      // normal card scan (grant/deny access using its stored name).
      Serial.println("No key pressed - treating Master Card as a normal scan.");
      processAccessCheck();
    }

    return;
  }
  else
  {
    Serial.println("Normal Card - Entering Read Mode...");
    readMode();
  }
}

void checkGlobalReset()
{
  char key_pressed = keypad.getKey();
  if (key_pressed == 'D')
  {
    Serial.println("[!] Resetting System...");
    oledMessage("Resetting...");
    delay(300);
    ESP.restart();
  }
}

void connectWiFi()
{
  Serial.print("Connecting to WiFi");
  oledMessage("Connecting to", "WiFi: " + String(WIFI_SSID));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000)
  {
    delay(400);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println();
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
    oledMessage("WiFi Connected", WiFi.localIP().toString());
    delay(1000);
  }
  else
  {
    Serial.println();
    Serial.println("WiFi connection failed - continuing offline. Sheet logging will be skipped.");
    oledMessage("WiFi Failed", "Running offline");
    delay(1000);
  }
}

String urlEncode(String str)
{
  String encoded = "";
  char c;
  char code0, code1;
  for (int i = 0; i < str.length(); i++)
  {
    c = str.charAt(i);
    if (isalnum(c))
    {
      encoded += c;
    }
    else
    {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9)
        code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9)
        code0 = c - 10 + 'A';
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

void sendToGoogleSheet(String name, String uidStr, String accessStatus)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected - skipping sheet log.");
    return;
  }

  if (String(GOOGLE_SCRIPT_URL).indexOf("YOUR_DEPLOYMENT_ID") >= 0)
  {
    Serial.println("Google Script URL not set - skipping sheet log.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // skips certificate validation, fine for this use case

  HTTPClient http;
  String url = String(GOOGLE_SCRIPT_URL) +
               "?name=" + urlEncode(name) +
               "&uid=" + urlEncode(uidStr) +
               "&status=" + urlEncode(accessStatus);

  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode > 0)
  {
    Serial.print("Sheet log response code: ");
    Serial.println(httpCode);
  }
  else
  {
    Serial.print("Sheet log failed: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
}

// Core grant/deny logic, assumes a card is ALREADY present, authenticated-
// ready (i.e. PICC_ReadCardSerial has just succeeded and it hasn't been
// halted yet). Reads the block, checks the name against the authorized
// list, gives buzzer/OLED feedback, logs to the sheet, then halts the card.
void processAccessCheck()
{
  ReadDataFromBlock(blockNum, readBlockData);

  char cardData[20] = "";
  int index = 0;
  for (int i = 0; i < 16; i++)
  {
    if (readBlockData[i] != 0 && readBlockData[i] != 32)
      cardData[index++] = (char)readBlockData[i];
  }
  cardData[index] = '\0';

  Serial.print("Data on Card: ");
  Serial.println(cardData);

  String scannedName = String(cardData);

  bool authorized = false;
  for (int i = 0; i < noOfNames; i++)
  {
    if (scannedName == nameList[i])
    {
      authorized = true;
      break;
    }
  }

  if (authorized)
  {
    Serial.println("ACCESS GRANTED");
    oledMessage("ACCESS GRANTED", scannedName);
    digitalWrite(BUZZER, HIGH);
    delay(200);
    digitalWrite(BUZZER, LOW);
    sendToGoogleSheet(scannedName, uid, "Access Granted");
  }
  else
  {
    Serial.println("ACCESS DENIED");
    oledMessage("ACCESS DENIED", scannedName.length() > 0 ? scannedName : "Unknown card");
    digitalWrite(BUZZER, HIGH);
    delay(800);
    digitalWrite(BUZZER, LOW);
    sendToGoogleSheet(scannedName.length() > 0 ? scannedName : "Unknown", uid, "Access Denied");
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  delay(1500);
  oledIdleScreen();
}

// Normal-card entry point: waits for a (re-)tap, then delegates to
// processAccessCheck() for the actual grant/deny logic.
void readMode()
{
  oledMessage("Scan card...");

  unsigned long startTime = millis();
  while (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial())
  {
    checkGlobalReset();

    if (millis() - startTime > 5000)
    {
      Serial.println("Timeout: No card detected.");
      oledMessage("Timeout!", "No card detected");
      delay(1000);
      oledIdleScreen();
      return;
    }
  }

  processAccessCheck();
}

void writeMode()
{
  Serial.println("Enter Name using keypad, then press C to confirm:");
  oledMessage("Enter name:", "", "C=OK *=Clear");

  String name = "";
  bool nameEntered = false;

  while (!nameEntered)
  {
    char key_pressed = keypad.getKey();

    if (key_pressed)
    {
      if (key_pressed == 'D')
      {
        ESP.restart();
      }
      else if (key_pressed == 'C')
      {
        nameEntered = true;
      }
      else if (key_pressed == '*')
      {
        name = "";
        Serial.println("Name cleared.");
        oledMessage("Enter name:", "", "C=OK *=Clear");
      }
      else if (name.length() < 16)
      {
        name += key_pressed;
        Serial.print("Name: ");
        Serial.println(name);
        oledMessage("Enter name:", name, "C=OK *=Clear");
      }
    }
  }

  if (name.length() == 0)
  {
    Serial.println("Empty name - write cancelled.");
    oledMessage("Cancelled", "Empty name");
    delay(1000);
    oledIdleScreen();
    return;
  }

  byte blockData[16] = {0};
  name.getBytes(blockData, 16);

  Serial.println("Name received. Now scan card to write...");
  oledMessage("Scan card to", "write: " + name);

  mfrc522.PCD_AntennaOff();
  delay(50);
  mfrc522.PCD_AntennaOn();
  delay(50);

  while (!mfrc522.PICC_IsNewCardPresent())
  {
    checkGlobalReset();
  }
  while (!mfrc522.PICC_ReadCardSerial())
    ;

  WriteDataToBlock(blockNum, blockData);
  Serial.println("Write Successful!");
  oledMessage("Write Success!", name);

  addNameToList(name); // now also saves to flash

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  delay(1500);
  oledIdleScreen();
}

void deleteMode()
{
  Serial.println("Scan the card you want to erase...");
  oledMessage("Scan card to", "erase...");

  mfrc522.PCD_AntennaOff();
  delay(50);
  mfrc522.PCD_AntennaOn();
  delay(50);

  unsigned long startTime = millis();
  while (!mfrc522.PICC_IsNewCardPresent())
  {
    checkGlobalReset();
    if (millis() - startTime > 10000)
    {
      Serial.println("Timeout: No card detected. Delete cancelled.");
      oledMessage("Timeout!", "Delete cancelled");
      delay(1000);
      oledIdleScreen();
      return;
    }
  }
  while (!mfrc522.PICC_ReadCardSerial())
    ;

  ReadDataFromBlock(blockNum, readBlockData);

  char cardData[20] = "";
  int index = 0;
  for (int i = 0; i < 16; i++)
  {
    if (readBlockData[i] != 0 && readBlockData[i] != 32)
      cardData[index++] = (char)readBlockData[i];
  }
  cardData[index] = '\0';
  String cardName = String(cardData);

  Serial.print("Name found on card: ");
  Serial.println(cardName);

  byte emptyBlock[16] = {0};
  WriteDataToBlock(blockNum, emptyBlock);
  Serial.println("Card data erased.");

  if (cardName.length() > 0 && removeNameFromList(cardName))
  {
    Serial.print("Removed \"");
    Serial.print(cardName);
    Serial.println("\" from authorized list.");
    oledMessage("Erased & Removed", cardName);
  }
  else
  {
    Serial.println("No matching name found in list (list unchanged).");
    oledMessage("Card Erased", "Not in list");
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  delay(1500);
  oledIdleScreen();
}

void loadNamesFromFlash()
{
  prefs.begin("rfid-auth", false);

  noOfNames = prefs.getInt("count", -1);

  if (noOfNames == -1)
  {
    String defaults[1] = {"varun"};
    noOfNames = 1;
    for (int i = 0; i < noOfNames; i++)
      nameList[i] = defaults[i];
    saveNamesToFlash();
  }
  else
  {
    for (int i = 0; i < noOfNames; i++)
    {
      String keyName = "name" + String(i);
      nameList[i] = prefs.getString(keyName.c_str(), "");
    }
  }

  prefs.end();
}

void saveNamesToFlash()
{
  prefs.begin("rfid-auth", false);
  prefs.putInt("count", noOfNames);
  for (int i = 0; i < noOfNames; i++)
  {
    String keyName = "name" + String(i);
    prefs.putString(keyName.c_str(), nameList[i]);
  }
  prefs.end();
}

void addNameToList(String newName)
{
  for (int i = 0; i < noOfNames; i++)
  {
    if (nameList[i] == newName)
    {
      Serial.println("Name already in authorized list.");
      return;
    }
  }

  if (noOfNames >= MAX_NAMES)
  {
    Serial.println("Name list full - could not add new name.");
    return;
  }

  nameList[noOfNames] = newName;
  noOfNames++;
  saveNamesToFlash();

  Serial.print("Added \"");
  Serial.print(newName);
  Serial.println("\" to authorized list (saved to flash).");
}

bool removeNameFromList(String targetName)
{
  for (int i = 0; i < noOfNames; i++)
  {
    if (nameList[i] == targetName)
    {
      for (int j = i; j < noOfNames - 1; j++)
        nameList[j] = nameList[j + 1];
      noOfNames--;
      saveNamesToFlash();
      return true;
    }
  }
  return false;
}

void WriteDataToBlock(int blockNum, byte blockData[])
{
  status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockNum, &key, &(mfrc522.uid));
  if (status == MFRC522::STATUS_OK)
  {
    mfrc522.MIFARE_Write(blockNum, blockData, 16);
  }
  else
  {
    Serial.println("Auth failed");
    oledMessage("Auth Failed!");
    delay(1000);
  }
}

void ReadDataFromBlock(int blockNum, byte readBlockData[])
{
  byte size = 18;
  status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockNum, &key, &(mfrc522.uid));
  if (status == MFRC522::STATUS_OK)
  {
    mfrc522.MIFARE_Read(blockNum, readBlockData, &size);
  }
  else
  {
    Serial.println("Auth failed");
    oledMessage("Auth Failed!");
    delay(1000);
  }
}

// ---------- OLED helper functions ----------

// Prints up to 3 lines of text on the OLED, clearing the screen first.
void oledMessage(String line1, String line2, String line3)
{
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println(line1);

  if (line2.length() > 0)
  {
    display.setCursor(0, 20);
    display.println(line2);
  }

  if (line3.length() > 0)
  {
    display.setCursor(0, 40);
    display.println(line3);
  }

  display.display();
}

// Default idle/standby screen shown when the system is waiting for a card.
void oledIdleScreen()
{
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("RFID Access System");
  display.setCursor(0, 16);
  display.println(WiFi.status() == WL_CONNECTED ? "WiFi: Connected" : "WiFi: Offline");
  display.setCursor(0, 32);
  display.println("Scan your card...");
  display.setCursor(0, 50);
  display.print("Users: ");
  display.println(noOfNames);
  display.display();
}

// One-time boot banner shown briefly at startup.
void oledSplashScreen()
{
  display.clearDisplay();

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Hello, World!");

  display.setTextSize(2);
  display.setCursor(0, 20);
  display.println("RFID Lock");

  display.display();
}