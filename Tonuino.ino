#include <DFMiniMp3.h>
#include <EEPROM.h>
#include <JC_Button.h>
#include <MFRC522.h>
#include <SPI.h>
#include <SoftwareSerial.h>
#include <avr/sleep.h>

/*
   _____         _____ _____ _____ _____
  |_   _|___ ___|  |  |     |   | |     |
    | | | . |   |  |  |-   -| | | |  |  |
    |_| |___|_|_|_____|_____|_|___|_____|
    TonUINO Version 2.1

    created by Thorsten Voß and licensed under GNU/GPL.
    Information and contribution at https://tonuino.de.
*/

// uncomment the below line to enable five button support
//#define FIVEBUTTONS

static const uint32_t cardCookie = 322417479;

// some MP3 modules cause two OnPlayFinished() calls when a track ends. -> set to 1
// if playback stops after each track -> set to 0
#define FILTER_DUPLICATE_ONPLAYFINISHED 1

// LED on D5 (high is off)
class LightController
{
public:
  LightController() : _isOn(false), _pwm(0), _hover(false) {};

  void init()
  {
    pinMode(5, OUTPUT);
    on();
  }
  
  void off()
  {
    _isOn = false;
    _hover = false;
    _pwm = 0;
    write();
  }
  
  void on()
  {
    _isOn = true;
    _hover = false;
    _pwm = 255;
    write();
  }

  void fade_on()
  {
    _isOn = true;
    _hover = false;
  }
  
  void fade_off()
  {
    _isOn = false;
    _hover = false;
  }
  
  void hover()
  {
    _hover = true;
  }
  
  void periodic()
  {        
    const byte steps = 8;
    static uint8_t lastStep = 0;
    uint8_t now = millis();
        
    if (static_cast<uint8_t>(now - lastStep) < 20) return;
    lastStep = now;
  
    if (_isOn)
    {
       _pwm = (_pwm < 255 - steps*2) ? _pwm + steps : 255;
       if (_pwm == 255 && _hover) _isOn = false;
    }
    else
    {
       _pwm = (_pwm > steps * 2) ? _pwm - steps : 0;
       if (_pwm == 0 && _hover) _isOn = true;
    }
    write();
  }
  
private:
  void write()
  {
	// apply some fake gamma curve
	uint8_t outval = (static_cast<uint16_t>(_pwm)* static_cast<uint16_t>(_pwm)) >> 8;
    analogWrite(5, 255 - outval);	// invert, because LED is on if pin is low.
  }
  
  bool _isOn;
  byte _pwm;
  bool _hover;
};

LightController light;


// Werte nicht ändern, die sind auf den Karten gespeichert!
enum AbspielModus
{
    Uninitialized   = 0,
    Hoerspiel       = 1,  // eine zufällige Datei aus dem Ordner
    Album           = 2,  // kompletten Ordner spielen
    Party           = 3,  // Ordner in zufälliger Reihenfolge
    EinzelTitel     = 4,  // eine Datei aus dem Ordner abspielen
    Hoerbuch        = 5,  // kompletten Ordner spielen und Fortschritt merken
    Admin           = 6,
    HoerspielRandom = 7,  // Spezialmodus Von-Bis: Hörspiel: eine zufällige Datei aus dem Ordner
    SpezialVonBis   = 8,  // Spezialmodus Von-Bis: Album: alle Dateien zwischen Start und Ende spielen
    PartyRandom     = 9   // Spezialmodus Von-Bis: Party Ordner in zufälliger Reihenfolge
};


// DFPlayer Mini
SoftwareSerial mySoftwareSerial(2, 3); // RX, TX
uint16_t numTracksInFolder;
uint16_t currentTrack;
uint16_t firstTrack;
uint8_t queue[255];
uint8_t volume;

struct folderSettings {
  uint8_t folder;
  uint8_t mode;
  uint8_t special;
  uint8_t special2;
};

// this object stores nfc tag data
struct nfcTagObject {
  uint32_t cookie;
  uint8_t version;
  folderSettings nfcFolderSettings;
};

// admin settings stored in eeprom
struct adminSettings {
  uint32_t cookie;
  byte version;
  uint8_t maxVolume;
  uint8_t minVolume;
  uint8_t initVolume;
  uint8_t eq;
  bool locked;
  long standbyTimer;
  bool invertVolumeButtons;
  folderSettings shortCuts[4];
};

adminSettings mySettings;
nfcTagObject myCard;
folderSettings *myFolder;
unsigned long sleepAtMillis = 0;

void powerOff();
static void nextTrack(uint16_t track);
uint8_t voiceMenu(int numberOfOptions, int startMessage, int messageOffset,
                  bool preview = false, int previewFromFolder = 0, int defaultValue = 0, bool exitWithLongPress = false);
bool isPlaying();
bool knownCard = false;

uint8_t tracksLeftBeforePowerOff = 0;	// disabled if == 0,  if > 0, powerOff() will be called after 'tracksLeftBeforePowerOff' tracks have finished playing.

#if FILTER_DUPLICATE_ONPLAYFINISHED
uint16_t g_lastTrackDone = 65535;
#endif

// implement a notification class,
// its member methods will get called
//
class Mp3Notify {
  public:
    static void OnError(uint16_t errorCode) {
      // see https://github.com/Makuna/DFMiniMp3/wiki/Notification-Method for code meaning
      Serial.print(F("DFPlayer Error "));
      Serial.println(errorCode);
    }
    static void printSource(DfMp3_PlaySources source) {
      if (source & DfMp3_PlaySources_Sd)    { Serial.print(F("SD Karte "));  return; }
      if (source & DfMp3_PlaySources_Usb)   { Serial.print(F("USB "));       return; }
      if (source & DfMp3_PlaySources_Flash) { Serial.print(F("Flash "));     return; }
            								                  Serial.print(F("Unbekannt "));
    }
	
/*	
OnPlayFinished source 2 g_lastTrackDone 65535 track 79	<- ignore
Now idle.
MP3 rx: 7E FF 6 3D 0 0 4F FE 6F EF <end>
OnPlayFinished source 2 g_lastTrackDone 79 track 79
=== nextTrack()
Party -> weiter in der Queue 1
MP3 TX: 7E FF 6 F 0 8 1 FE E3 EF <end>
Now busy.
*/

    static void OnPlayFinished(DfMp3_PlaySources source, uint16_t track) {
            Serial.print(F("OnPlayFinished source "));
            printSource(source);
#if FILTER_DUPLICATE_ONPLAYFINISHED
            Serial.print(F(" g_lastTrackDone "));
            Serial.print(g_lastTrackDone);
#endif
            Serial.print(F(" track "));
            Serial.println(track);
	  
	  isPlaying();	// just for logging

#if FILTER_DUPLICATE_ONPLAYFINISHED
	  // https://github.com/Makuna/DFMiniMp3/wiki/Notification-Method
	  // If you play a single track, you should get called twice, one for the finish of the track, 
	  // and another for the finish of the command. This is a nuance of the chip.
	  if (track != g_lastTrackDone)
	  {
		g_lastTrackDone = track;
		return;
	  }
#endif
	  
	  if (tracksLeftBeforePowerOff > 0)
	  {
            Serial.print(F("tracksLeftBeforePowerOff "));
            Serial.print((uint16_t)tracksLeftBeforePowerOff);
		  tracksLeftBeforePowerOff--;
		  if (tracksLeftBeforePowerOff == 0)
		  {
			  powerOff();
		  }
	  }
      nextTrack(track);
	  
	    isPlaying();	// just for logging
    }
    static void OnPlaySourceOnline(DfMp3_PlaySources source) {
      printSource(source);
      Serial.println(F("online"));
    }
    static void OnPlaySourceInserted(DfMp3_PlaySources source) {
      printSource(source);
      Serial.println(F("bereit"));
    }
    static void OnPlaySourceRemoved(DfMp3_PlaySources source) {
      printSource(source);
      Serial.println(F("entfernt"));
    }
};

static DFMiniMp3<SoftwareSerial, Mp3Notify> mp3(mySoftwareSerial);

void shuffleQueue() {
  // Queue für die Zufallswiedergabe erstellen
  for (uint8_t x = 0; x < numTracksInFolder - firstTrack + 1; x++)
    queue[x] = x + firstTrack;
  // Rest mit 0 auffüllen
  for (uint8_t x = numTracksInFolder - firstTrack + 1; x < 255; x++)
    queue[x] = 0;
  // Queue mischen
  for (uint8_t i = 0; i < numTracksInFolder - firstTrack + 1; i++)
  {
    uint8_t j = random (0, numTracksInFolder - firstTrack + 1);
    uint8_t t = queue[i];
    queue[i] = queue[j];
    queue[j] = t;
  }
}

void writeSettingsToFlash() {
  Serial.println(F("=== writeSettingsToFlash()"));
  int address = sizeof(myFolder->folder) * 100;
  EEPROM.put(address, mySettings);
}

void resetSettings() {
  Serial.println(F("=== resetSettings()"));
  mySettings.cookie = cardCookie;
  mySettings.version = 1;
  mySettings.maxVolume = 25;
  mySettings.minVolume = 5;
  mySettings.initVolume = 15;
  mySettings.eq = 1;
  mySettings.locked = false;
  mySettings.standbyTimer = 0;
  mySettings.invertVolumeButtons = true;
  mySettings.shortCuts[0].folder = 0;
  mySettings.shortCuts[1].folder = 0;
  mySettings.shortCuts[2].folder = 0;
  mySettings.shortCuts[3].folder = 0;
  writeSettingsToFlash();
}

void migrateSettings(int oldVersion) {

}

void loadSettingsFromFlash() {
  Serial.println(F("=== loadSettingsFromFlash()"));
  int address = sizeof(myFolder->folder) * 100;
  EEPROM.get(address, mySettings);
  if (mySettings.cookie != cardCookie)
    resetSettings();
  migrateSettings(mySettings.version);

  Serial.print(F("Version: "));
  Serial.println(mySettings.version);

  Serial.print(F("Maximal Volume: "));
  Serial.println(mySettings.maxVolume);

  Serial.print(F("Minimal Volume: "));
  Serial.println(mySettings.minVolume);

  Serial.print(F("Initial Volume: "));
  Serial.println(mySettings.initVolume);

  Serial.print(F("EQ: "));
  Serial.println(mySettings.eq);

  Serial.print(F("Locked: "));
  Serial.println(mySettings.locked);

  Serial.print(F("Sleep Timer: "));
  Serial.println(mySettings.standbyTimer);

  Serial.print(F("Inverted Volume Buttons: "));
  Serial.println(mySettings.invertVolumeButtons);
}

void onQueueEmpty()
{
  setstandbyTimer();
  light.hover();
  forgetCurrentCard();
}

// Leider kann das Modul selbst keine Queue abspielen, daher müssen wir selbst die Queue verwalten
static uint16_t _lastTrackFinished;
static void nextTrack(uint16_t track) {
  if (track == _lastTrackFinished) {
    return;
  }
  _lastTrackFinished = track;

  if (!knownCard)
    // Wenn eine neue Karte angelernt wird soll das Ende eines Tracks nicht
    // verarbeitet werden
    return;

  Serial.println(F("=== nextTrack()"));
  
  switch (myFolder->mode)
  {    
  case Hoerspiel:
  case HoerspielRandom:
    Serial.println(F("Hörspielmodus ist aktiv -> keinen neuen Track spielen"));
    onQueueEmpty();
  break;
  
  case Album:
  case SpezialVonBis:
    if (currentTrack != numTracksInFolder) {
      currentTrack++;
      mp3.playFolderTrack(myFolder->folder, currentTrack);
      Serial.print(F("Albummodus ist aktiv -> nächster Track: "));
      Serial.print(currentTrack);
    } else
      onQueueEmpty();
    break;
  
  case Party:
  case PartyRandom:
    if (currentTrack != numTracksInFolder - firstTrack + 1) {
      Serial.print(F("Party -> weiter in der Queue "));
      currentTrack++;
    } else {
      Serial.println(F("Ende der Queue -> beginne von vorne"));
      currentTrack = 1;
      //// Wenn am Ende der Queue neu gemischt werden soll bitte die Zeilen wieder aktivieren
      //     Serial.println(F("Ende der Queue -> mische neu"));
      //     shuffleQueue();
    }
    Serial.println(queue[currentTrack - 1]);
    mp3.playFolderTrack(myFolder->folder, queue[currentTrack - 1]);
  break;

  case EinzelTitel:
    Serial.println(F("Einzel Modus aktiv -> Strom sparen"));
    onQueueEmpty();
    break;
  
  case Hoerbuch:
    if (currentTrack != numTracksInFolder) {
      currentTrack++;
      Serial.print(F("Hörbuch Modus ist aktiv -> nächster Track und Fortschritt speichern"));
      Serial.println(currentTrack);
      mp3.playFolderTrack(myFolder->folder, currentTrack);
      // Fortschritt im EEPROM abspeichern
      EEPROM.update(myFolder->folder, currentTrack);
    } else {
      // Fortschritt zurücksetzen
      EEPROM.update(myFolder->folder, 1);
      onQueueEmpty();
    }
  break;
  }
  delay(500);
}

static void previousTrack() {
  Serial.println(F("=== previousTrack()"));
  /*  if (myFolder->mode == Hoerspiel || myFolder->mode == HoerspielRandom) {
      Serial.println(F("Hörspielmodus ist aktiv -> Track von vorne spielen"));
      mp3.playFolderTrack(myCard.folder, currentTrack);
    }*/
  switch (myFolder->mode)
  {
  case Album:
  case SpezialVonBis:
    Serial.println(F("Albummodus ist aktiv -> vorheriger Track"));
    if (currentTrack != firstTrack)
      currentTrack--;
    
    mp3.playFolderTrack(myFolder->folder, currentTrack);
  break;
  
  case Party:
  case PartyRandom:
    if (currentTrack != 1) {
      Serial.print(F("Party Modus ist aktiv -> zurück in der Queue "));
      currentTrack--;
    }
    else
    {
      Serial.print(F("Anfang der Queue -> springe ans Ende "));
      currentTrack = numTracksInFolder;
    }
    Serial.println(queue[currentTrack - 1]);
    mp3.playFolderTrack(myFolder->folder, queue[currentTrack - 1]);
    break;
  
  case EinzelTitel:
    Serial.println(F("Einzel Modus aktiv -> Track von vorne spielen"));
    mp3.playFolderTrack(myFolder->folder, currentTrack);
  break;
  
  case Hoerbuch:
    Serial.println(F("Hörbuch Modus ist aktiv -> vorheriger Track und "
                     "Fortschritt speichern"));
    if (currentTrack != 1)
      currentTrack--;
    
    mp3.playFolderTrack(myFolder->folder, currentTrack);
    // Fortschritt im EEPROM abspeichern
    EEPROM.update(myFolder->folder, currentTrack);
    break;
  }
  delay(1000);
}

// MFRC522
#define RST_PIN 9                 // Configurable, see typical pin layout above
#define SS_PIN 10                 // Configurable, see typical pin layout above
MFRC522 mfrc522(SS_PIN, RST_PIN); // Create MFRC522
MFRC522::MIFARE_Key key;
const byte sector = 1;
const byte blockAddr = 4;
const byte trailerBlock = 7;
MFRC522::StatusCode status;

enum ButtonID
{
  Next,
  Prev,
  VolP,
  VolM
};


const uint8_t aButtonPin[] = { A0, A1, A2, A3 };
const uint8_t numButtons = sizeof(aButtonPin) / sizeof(*aButtonPin);

#define busyPin 4
#define shutdownPin 7
#define openAnalogPin A7

#define LONG_PRESS 1000


/// Funktionen für den Standby Timer (z.B. über Pololu-Switch oder Mosfet)

void setstandbyTimer() {
  Serial.println(F("=== setstandbyTimer()"));
  if (mySettings.standbyTimer != 0)
    sleepAtMillis = millis() + (mySettings.standbyTimer * 60 * 1000);
  else
    sleepAtMillis = 0;
}

void disablestandbyTimer() {
  Serial.println(F("=== disablestandby()"));
  sleepAtMillis = 0;
}

void powerOff()
{
    Serial.println(F("=== power off!"));
    light.off();
    // enter sleep state
    digitalWrite(shutdownPin, HIGH);
    delay(500);

    // http://discourse.voss.earth/t/intenso-s10000-powerbank-automatische-abschaltung-software-only/805
    // powerdown to 27mA (powerbank switches off after 30-60s)
    mfrc522.PCD_AntennaOff();
    mfrc522.PCD_SoftPowerDown();
    mp3.sleep();

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    cli();  // Disable interrupts
    sleep_mode();
}

void checkStandbyAtMillis() {
  if (sleepAtMillis != 0 && millis() > sleepAtMillis)
    powerOff();
}


const byte BTN_NO_PRESS    = 0; // nothing happened
const byte BTN_SHORT_PRESS = 1;	// button was shortly pressed and then released
const byte BTN_LONG_PRESS  = 2;	// button is pressed for a while and still pressed after LONG_PRESS duration
const byte BTN_LONG_REPEAT = 3;	// button has been pressed for another LONG_PRESS duration

// class to query whether the button was pressed for at least LONG_PRESS after the button was released
class ExtButton : public Button
{
public:
    ExtButton(ButtonID id) : Button(aButtonPin[id]), _longPressCount(1) {};

    byte readEvent()
    {
      read();
		  if (wasPressed())
		  {
		  	_longPressCount = 0;
		  }
      if (wasReleased())
      {
          bool bShort = (_longPressCount == 0);
          _longPressCount = 1;	// use 1 as marker to not send event
          if (bShort) return BTN_SHORT_PRESS;
      }
		  if (pressedFor(static_cast<uint16_t>(_longPressCount + 1) * LONG_PRESS))
      {
          return (_longPressCount++ == 0) ? BTN_LONG_PRESS : BTN_LONG_REPEAT;
      }
		return BTN_NO_PRESS;
	}
	
private:
    uint8_t    _longPressCount;
};


ExtButton button[numButtons] = { Next, Prev, VolP, VolM };

/*
	playAdvertisement
		-does not work directly after setVolume!
		
		(busy, while playing regular track)
		-> playAdvertisement()
		-> Now idle.
		-> Now busy (while playing advertisement)
		-> Now idle. (when done with advertisement)
		-> Now busy (resume track)

		(nothing playing or regular track paused)
		Return COM error 7, no playback
*/


bool g_bPaused = false;
bool g_lastBusy = true;

bool isPlaying() {
	bool busy = !digitalRead(busyPin);
	if (busy != g_lastBusy) Serial.println(busy ? F("Now busy.") : F("Now idle."));
	g_lastBusy = busy;
	return busy;
}

void waitForTrackToFinish() {
  long currentTime = millis();
  const int timeoutMs = 1000;
  do {
    mp3.loop();
  } while (!isPlaying() && millis() < currentTime + timeoutMs);
  delay(1000);
  do {
    mp3.loop();
  } while (isPlaying());
}

void setup() {

  Serial.begin(115200); // Es gibt ein paar Debug Ausgaben über die serielle Schnittstelle
   
  {
    // Wert für randomSeed() erzeugen durch das mehrfache Sammeln von rauschenden LSBs eines offenen Analogeingangs
    uint32_t ADCSeed;
    for(uint8_t i = 0; i < 128; i++) {
      uint32_t ADC_LSB = analogRead(openAnalogPin) & 0x1;
      ADCSeed ^= ADC_LSB << (i % 32);
    }
    randomSeed(ADCSeed); // Zufallsgenerator initialisieren
  }

  // Dieser Hinweis darf nicht entfernt werden
  Serial.println(F("\n _____         _____ _____ _____ _____"));
  Serial.println(F("|_   _|___ ___|  |  |     |   | |     |"));
  Serial.println(F("  | | | . |   |  |  |-   -| | | |  |  |"));
  Serial.println(F("  |_| |___|_|_|_____|_____|_|___|_____|\n"));
  Serial.println(F("TonUINO Version 2.1"));
  Serial.println(F("created by Thorsten Voß and licensed under GNU/GPL."));
  Serial.println(F("Information and contribution at https://tonuino.de.\n"));

  // Busy Pin
  pinMode(busyPin, INPUT);

  // load Settings from EEPROM
  loadSettingsFromFlash();

  // activate standby timer
  setstandbyTimer();
  
  light.init();

  // DFPlayer Mini initialisieren
  mp3.begin();
  // Zwei Sekunden warten bis der DFPlayer Mini initialisiert ist
  delay(500);
  light.off();
  delay(1500);
  
  setVolume(mySettings.initVolume);
  mp3.setEq(static_cast<DfMp3_Eq>(mySettings.eq - 1));
  // Fix für das Problem mit dem Timeout (ist jetzt in Upstream daher nicht mehr nötig!)
  //mySoftwareSerial.setTimeout(10000);

  // NFC Leser initialisieren
  SPI.begin();        // Init SPI bus
  mfrc522.PCD_Init(); // Init MFRC522
  mfrc522.PCD_DumpVersionToSerial(); // Show details of PCD - MFRC522 Card Reader
  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

  for (byte i = 0; i < numButtons; i++)
  {
    pinMode(aButtonPin[i], INPUT_PULLUP);
  }
  
  pinMode(shutdownPin, OUTPUT);
  digitalWrite(shutdownPin, LOW);

  // RESET --- ALLE DREI KNÖPFE BEIM STARTEN GEDRÜCKT HALTEN -> alle EINSTELLUNGEN werden gelöscht
    if (digitalRead(aButtonPin[VolP]) == LOW && digitalRead(aButtonPin[VolM]) == LOW &&
        digitalRead(aButtonPin[Prev]) == LOW && digitalRead(aButtonPin[Next]) == LOW) {
    Serial.println(F("Reset -> EEPROM wird gelöscht"));
    for (int i = 0; i < EEPROM.length(); i++) {
      EEPROM.update(i, 0);
    }
  }
  // Start Shortcut "at Startup" - e.g. Welcome Sound
  playShortCut(3);
  light.hover();
}

byte btnEv[numButtons] = {0};

void readButtons() {
  for (byte i = 0; i < numButtons; i++)
  {
    btnEv[i] = button[i].readEvent();
  }
}

void setVolume(uint8_t volnew)
{
  Serial.print(F("set volume "));
  Serial.println(volnew);
  mp3.setVolume(volnew);
  volume = volnew;
}

void changeVolume(char delta)
{
  int16_t volnew = volume + delta;
  if (volnew > mySettings.maxVolume) volnew = mySettings.maxVolume;
  if (volnew < mySettings.minVolume) volnew = mySettings.minVolume;
  if (volnew != volume)
  {
    setVolume(volnew);
  }
}

void nextButton() {
  if (g_bPaused)
  {
	light.off();
    mp3.start();
    g_bPaused = false;
    disablestandbyTimer();
  }
  else
  {
    nextTrack(random(65536));
    delay(1000);
  }
}

void previousButton() {
  if (g_bPaused)
  {
    mp3.start();
    g_bPaused = false;
    disablestandbyTimer();
  }
  else
  {
    previousTrack();
    delay(1000);
  }
}

void playFolder() {
  disablestandbyTimer();
  knownCard = true;
  _lastTrackFinished = 0;
  numTracksInFolder = mp3.getFolderTrackCount(myFolder->folder);
  firstTrack = 1;
  Serial.print(numTracksInFolder);
  Serial.print(F(" Dateien in Ordner "));
  Serial.println(myFolder->folder);

  switch (myFolder->mode)
  {
  case Hoerspiel:
    Serial.println(F("Hörspielmodus -> zufälligen Track wiedergeben"));
    currentTrack = random(1, numTracksInFolder + 1);
    Serial.println(currentTrack);
    mp3.playFolderTrack(myFolder->folder, currentTrack);
    break;
  
  case Album:
    Serial.println(F("Album Modus -> kompletten Ordner wiedergeben"));
    currentTrack = 1;
    mp3.playFolderTrack(myFolder->folder, currentTrack);
    break;
  
  case Party:
    Serial.println(F("Party Modus -> Ordner in zufälliger Reihenfolge wiedergeben"));
    shuffleQueue();
    currentTrack = 1;
    mp3.playFolderTrack(myFolder->folder, queue[currentTrack - 1]);
    break;
  
  case EinzelTitel:
    Serial.println(F("Einzel Modus -> eine Datei aus dem Odrdner abspielen"));
    currentTrack = myFolder->special;
    mp3.playFolderTrack(myFolder->folder, currentTrack);
    break;
  
  case Hoerbuch:
    Serial.println(F("Hörbuch Modus -> kompletten Ordner spielen und Fortschritt merken"));
    currentTrack = EEPROM.read(myFolder->folder);
    if (currentTrack == 0 || currentTrack > numTracksInFolder) {
      currentTrack = 1;
    }
    mp3.playFolderTrack(myFolder->folder, currentTrack);
    break;
  
  case HoerspielRandom:
    Serial.println(F("Spezialmodus Von-Bis: Hörspiel -> zufälligen Track wiedergeben"));
    Serial.print(myFolder->special);
    Serial.print(F(" bis "));
    Serial.println(myFolder->special2);
    numTracksInFolder = myFolder->special2;
    currentTrack = random(myFolder->special, numTracksInFolder + 1);
    Serial.println(currentTrack);
    mp3.playFolderTrack(myFolder->folder, currentTrack);
    break;

  case SpezialVonBis:
    Serial.println(F("Spezialmodus Von-Bis: Album: alle Dateien zwischen Start- und Enddatei spielen"));
    Serial.print(myFolder->special);
    Serial.print(F(" bis "));
    Serial.println(myFolder->special2);
    numTracksInFolder = myFolder->special2;
    currentTrack      = myFolder->special;
    mp3.playFolderTrack(myFolder->folder, currentTrack);
    break;

  case PartyRandom:
    Serial.println(F("Spezialmodus Von-Bis: Party -> Ordner in zufälliger Reihenfolge wiedergeben"));
    firstTrack = myFolder->special;
    numTracksInFolder = myFolder->special2;
    shuffleQueue();
    currentTrack = 1;
    mp3.playFolderTrack(myFolder->folder, queue[currentTrack - 1]);
    break;
  }
}

void playShortCut(uint8_t shortCut) {
  Serial.print(F("=== playShortCut() "));
  Serial.println(shortCut);
  if (mySettings.shortCuts[shortCut].folder != 0) {
    myFolder = &mySettings.shortCuts[shortCut];
    playFolder();
    disablestandbyTimer();
    delay(1000);
  }
  else
    Serial.println(F("Shortcut not configured!"));
}


static bool hasCard = false;

static byte lastCardUid[4];
static byte retries;
static bool lastCardWasUL;

void forgetCurrentCard()
{
  for (byte i = 0; i < 4; i++)
      lastCardUid[i] = 0;
  hasCard   = false;
  g_bPaused = false;
}

const byte PCS_NO_CHANGE	 = 0; // no change detected since last pollCard() call
const byte PCS_NEW_CARD 	 = 1; // card with new UID detected (had no card or other card before)
const byte PCS_CARD_GONE     = 2;// card is not reachable anymore
const byte PCS_CARD_IS_BACK  = 3;// card was gone, and is now back again


byte pollCard()
{
	const byte maxRetries = 2;

	if (!hasCard)
  {
		if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial() && readCard(myCard))
    {
      bool bSameUID = !memcmp(lastCardUid, mfrc522.uid.uidByte, 4);
      Serial.print(F("IsSameAsLastUID="));
      Serial.println(bSameUID);
      // store info about current card
      memcpy(lastCardUid, mfrc522.uid.uidByte, 4);
      lastCardWasUL = mfrc522.PICC_GetType(mfrc522.uid.sak) == MFRC522::PICC_TYPE_MIFARE_UL;
    
      retries = maxRetries;
      hasCard = true;
      return bSameUID ? PCS_CARD_IS_BACK : PCS_NEW_CARD;
    }
    return PCS_NO_CHANGE;
  }
	else // hasCard
	{
        // perform a dummy read command just to see whether the card is in range
		byte buffer[18];
		byte size = sizeof(buffer);
		
		if (mfrc522.MIFARE_Read(lastCardWasUL ? 8 : blockAddr, buffer, &size) != MFRC522::STATUS_OK)
    {
			if (retries > 0)
      {
				retries--;
			}
			else
      {
				Serial.println(F("card gone"));
				mfrc522.PICC_HaltA();
				mfrc522.PCD_StopCrypto1();
				hasCard = false;
				return PCS_CARD_GONE;
      }
    }
    else
    {
        retries = maxRetries;
    }
  }
	return PCS_NO_CHANGE;
}

void handleCardReader()
{
  // poll card only every 100ms
  static uint8_t lastCardPoll = 0;
  uint8_t now = millis();
  
  if (static_cast<uint8_t>(now - lastCardPoll) > 100)
  {
    lastCardPoll = now;
    switch (pollCard())
    {
    case PCS_NEW_CARD:  
      g_bPaused = false;
      onNewCard();
      light.fade_off();
      break;
      
    case PCS_CARD_GONE:
      if (!g_bPaused)
        mp3.pause();
      g_bPaused = true;
      setstandbyTimer();
      light.hover();
      break;
      
    case PCS_CARD_IS_BACK:
      if (g_bPaused)
        mp3.start();
      g_bPaused = false;
      disablestandbyTimer();
      light.fade_off();
      break;
    }    
  }
}

void sleepModeButton()
{
	mp3.playAdvertisement(5);
	tracksLeftBeforePowerOff++;
}


void loop() {

  isPlaying();
  light.periodic();
  checkStandbyAtMillis();
  mp3.loop();
  readButtons();
  
  // admin menu
  if ((button[VolP].pressedFor(LONG_PRESS) || button[Next].pressedFor(LONG_PRESS)) && 
       button[VolP].isPressed() && button[Next].isPressed())
  {
    mp3.pause();
    do {
      readButtons();
    } while (button[VolP].isPressed() || button[Next].isPressed());
    readButtons();
    forgetCurrentCard();
    adminMenu();
    return;
  }
  
  // playAdvertisement only works when regular track is playing already!
  switch (btnEv[VolP])
  {
  case BTN_SHORT_PRESS: Serial.println(F("Vol+ short")); changeVolume(+3); 	 break;
  case BTN_LONG_PRESS:  Serial.println(F("Vol+ long"));  					 break;
  }
  switch (btnEv[VolM])
  {
  case BTN_SHORT_PRESS: Serial.println(F("Vol- short")); changeVolume(-3); 	 break;
  case BTN_LONG_PRESS:  Serial.println(F("Vol- long"));  powerOff();         break;
  }
  switch (btnEv[Prev])
  {
  case BTN_SHORT_PRESS: Serial.println(F("Prev short")); previousButton();   break;
  case BTN_LONG_PRESS:  Serial.println(F("Prev long"));  sleepModeButton();  break;
  }
  switch (btnEv[Next])
  {
  case BTN_SHORT_PRESS: Serial.println(F("Next short")); nextButton();       break;
  case BTN_LONG_PRESS:  Serial.println(F("Next long"));                      break;
  }
  // Ende der Buttons

  handleCardReader();
}

void onNewCard()
{    
  if (myCard.cookie == cardCookie && myFolder->folder != 0 && myFolder->mode != Uninitialized)
  {
    playFolder();
  }
  else
  { 
	  // Neue Karte konfigurieren
    knownCard = false;
    mp3.playMp3FolderTrack(300);
    waitForTrackToFinish();
    setupCard();
  }
}

void adminMenu() {
  disablestandbyTimer();
  mp3.pause();
  Serial.println(F("=== adminMenu()"));
  knownCard = false;

  int subMenu = voiceMenu(11, 900, 900, false, false, 0, true);
  if (subMenu == 0)
    return;
  if (subMenu == 1) {
    resetCard();
    return;
  }
  else if (subMenu == 2) {
    // Maximum Volume
    mySettings.maxVolume = voiceMenu(30, 930, 0, false, false, mySettings.maxVolume);
  }
  else if (subMenu == 3) {
    // Minimum Volume
    mySettings.minVolume = voiceMenu(30, 931, 0, false, false, mySettings.minVolume);
  }
  else if (subMenu == 4) {
    // Initial Volume
    mySettings.initVolume = voiceMenu(30, 932, 0, false, false, mySettings.initVolume);
  }
  else if (subMenu == 5) {
    // EQ
    mySettings.eq = voiceMenu(6, 920, 920, false, false, mySettings.eq);
    mp3.setEq(static_cast<DfMp3_Eq>(mySettings.eq - 1));
  }
  else if (subMenu == 6) {
    // create master card
  }
  else if (subMenu == 7) {
    uint8_t shortcut = voiceMenu(4, 940, 940);
    setupFolder(&mySettings.shortCuts[shortcut - 1]);
    mp3.playMp3FolderTrack(400);
  }
  else if (subMenu == 8) {
      const byte aStandbyTimer[] = { 5, 15, 30, 60, 0};	// TODO: PROGMEM
      mySettings.standbyTimer = aStandbyTimer[voiceMenu(5, 960, 960) - 1];
  }
  else if (subMenu == 9) {
    // Create Cards for Folder
    // Ordner abfragen
    nfcTagObject tempCard;
    tempCard.cookie = cardCookie;
    tempCard.version = 1;
    tempCard.nfcFolderSettings.mode = 4;
    tempCard.nfcFolderSettings.folder = voiceMenu(99, 301, 0, true);
    uint8_t special = voiceMenu(mp3.getFolderTrackCount(tempCard.nfcFolderSettings.folder), 321, 0,
                                true, tempCard.nfcFolderSettings.folder);
    uint8_t special2 = voiceMenu(mp3.getFolderTrackCount(tempCard.nfcFolderSettings.folder), 322, 0,
                                 true, tempCard.nfcFolderSettings.folder, special);

    mp3.playMp3FolderTrack(936);
    waitForTrackToFinish();
    for (uint8_t x = special; x <= special2; x++) {
      mp3.playMp3FolderTrack(x);
      tempCard.nfcFolderSettings.special = x;
      Serial.print(x);
      Serial.println(F(" Karte auflegen"));
      do {
        readButtons();
        if (btnEv[Prev] == BTN_SHORT_PRESS || btnEv[Next] == BTN_SHORT_PRESS)
        {
          Serial.println(F("Abgebrochen!"));
          mp3.playMp3FolderTrack(802);
          return;
        }
      } while (!mfrc522.PICC_IsNewCardPresent());

      // RFID Karte wurde aufgelegt
      if (!mfrc522.PICC_ReadCardSerial())
        return;
      Serial.println(F("schreibe Karte..."));
      writeCard(tempCard);
      delay(100);
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      waitForTrackToFinish();
    }
  }
  else if (subMenu == 10) {
    // Invert Functions for Up/Down Buttons
    int temp = voiceMenu(2, 933, 933, false);
    mySettings.invertVolumeButtons = (temp == 2);
  }
  else if (subMenu == 11) {
    Serial.println(F("Reset -> EEPROM wird gelöscht"));
    for (int i = 0; i < EEPROM.length(); i++) {
      EEPROM.update(i, 0);
    }
    resetSettings();
    mp3.playMp3FolderTrack(999);
  }
  writeSettingsToFlash();
  setstandbyTimer();
}

uint8_t voiceMenu(int numberOfOptions, int startMessage, int messageOffset,
                  bool preview, int previewFromFolder, int defaultValue, bool exitWithLongPress)
{
  uint8_t returnValue = defaultValue;
  if (startMessage != 0)
    mp3.playMp3FolderTrack(startMessage);
  Serial.print(F("=== voiceMenu() ("));
  Serial.print(numberOfOptions);
  Serial.println(F(" Options)"));
  do {
    if (Serial.available() > 0) {
      int optionSerial = Serial.parseInt();
      if (optionSerial != 0 && optionSerial <= numberOfOptions)
        return optionSerial;
    }
    readButtons();
    mp3.loop();

	switch (btnEv[Next])
	{
	case BTN_SHORT_PRESS:
        returnValue = min(returnValue + 1, numberOfOptions);
        Serial.println(returnValue);
        mp3.playMp3FolderTrack(messageOffset + returnValue);
        if (preview) {
          waitForTrackToFinish();
          if (previewFromFolder == 0) {
            mp3.playFolderTrack(returnValue, 1);
          } else {
            mp3.playFolderTrack(previewFromFolder, returnValue);
          }
          delay(1000);
        }
      break;
      
	case BTN_LONG_PRESS:
      returnValue = min(returnValue + 10, numberOfOptions);
      Serial.println(returnValue);
      mp3.playMp3FolderTrack(messageOffset + returnValue);
      waitForTrackToFinish();
	  break;
	}
	
	switch (btnEv[Prev])
	{
	case BTN_SHORT_PRESS:
        returnValue = max(returnValue - 1, 1);
        Serial.println(returnValue);
        mp3.playMp3FolderTrack(messageOffset + returnValue);
        if (preview) {
          waitForTrackToFinish();
          if (previewFromFolder == 0) {
            mp3.playFolderTrack(returnValue, 1);
          }
          else {
            mp3.playFolderTrack(previewFromFolder, returnValue);
          }
          delay(1000);
        }
      break;
	  
	case BTN_LONG_PRESS:
      returnValue = max(returnValue - 10, 1);
      Serial.println(returnValue);
      mp3.playMp3FolderTrack(messageOffset + returnValue);
      waitForTrackToFinish();
	  break;
	}
	
	switch (btnEv[VolP])
	{
	case BTN_SHORT_PRESS:
	  if (returnValue != 0) {
		  Serial.print(F("=== "));
		  Serial.print(returnValue);
		  Serial.println(F(" ==="));
		  return returnValue;
    }
	  delay(1000);
	  break;
	  
	case BTN_LONG_PRESS:
	  mp3.playMp3FolderTrack(802);
	  return 0;
    }
  } while (true);
}

void resetCard() {
  // allow connecting to other card
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  
  mp3.playMp3FolderTrack(800);
  do {
	  readButtons();
      if (btnEv[Prev] == BTN_SHORT_PRESS || btnEv[Next] == BTN_SHORT_PRESS) {
      Serial.print(F("Abgebrochen!"));
      mp3.playMp3FolderTrack(802);
      return;
    }
  } while (!mfrc522.PICC_IsNewCardPresent());

  if (!mfrc522.PICC_ReadCardSerial())
    return;

  Serial.print(F("Karte wird neu konfiguriert!"));
  setupCard();
}

bool setupFolder(folderSettings * theFolder) {
  // Ordner abfragen
  theFolder->folder = voiceMenu(99, 301, 0, true);
  if (theFolder->folder == 0) return false;

  // Wiedergabemodus abfragen
  theFolder->mode = voiceMenu(9, 310, 310);
  if (theFolder->mode == 0) return false;

  //  // Hörbuchmodus -> Fortschritt im EEPROM auf 1 setzen
  //  EEPROM.update(theFolder->folder, 1);

  switch (theFolder->mode)
  {
  // Einzelmodus -> Datei abfragen
  case EinzelTitel:
    theFolder->special = voiceMenu(mp3.getFolderTrackCount(theFolder->folder), 320, 0,
                                   true, theFolder->folder);
    break;

  case Admin:
    //theFolder->special = voiceMenu(3, 320, 320);
    theFolder->folder = 0;
    theFolder->mode = 255;
  break;

  case HoerspielRandom:
  case SpezialVonBis:
  case PartyRandom:
  theFolder->special = voiceMenu(mp3.getFolderTrackCount(theFolder->folder), 321, 0,
                                   true, theFolder->folder);
  theFolder->special2 = voiceMenu(mp3.getFolderTrackCount(theFolder->folder), 322, 0,
                                   true, theFolder->folder, theFolder->special);
  break;
  }
  return true;
}

void setupCard() {
  mp3.pause();
  Serial.println(F("=== setupCard()"));
  nfcTagObject newCard;
  if (setupFolder(&newCard.nfcFolderSettings))
  {
    // Karte ist konfiguriert -> speichern
    mp3.pause();
    while (isPlaying());
    writeCard(newCard);
    forgetCurrentCard();
  }
  delay(1000);
}

bool readCard(nfcTagObject& nfcTag) {
  // Show some details of the PICC (that is: the tag/card)
  Serial.print(F("Card UID:"));
  dump_byte_array(mfrc522.uid.uidByte, mfrc522.uid.size);
  Serial.println();
  Serial.print(F("PICC type: "));
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  Serial.println(mfrc522.PICC_GetTypeName(piccType));
  const bool bIsMifareUL = piccType == MFRC522::PICC_TYPE_MIFARE_UL;

  byte buffer[18+12];   // add more room at the end so that UL read with offset of up to 12 bytes fits
  byte size = 18;

  // Authenticate using key A
  if (!bIsMifareUL)
  {
    Serial.println(F("Authenticating Classic using key A..."));
    status = mfrc522.PCD_Authenticate(
               MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailerBlock, &key, &(mfrc522.uid));
  }
  else
  {
    byte pACK[] = {0, 0}; //16 bit PassWord ACK returned by the NFCtag

    Serial.println(F("Authenticating MIFARE UL..."));
    status = mfrc522.PCD_NTAG216_AUTH(key.keyByte, pACK);
  }

  if (status != MFRC522::STATUS_OK) {
    Serial.print(F("PCD_Authenticate() failed: "));
    Serial.println(mfrc522.GetStatusCodeName(status));
    return false;
  }

  // Read data from the block
  Serial.print(F("Reading data from block "));
  if (!bIsMifareUL)
  {
	// classic cards read 16 bytes at once
    status = mfrc522.MIFARE_Read(blockAddr, buffer, &size);
    if (status != MFRC522::STATUS_OK) {
      Serial.print(F("MIFARE_Read() failed: "));
      Serial.println(mfrc522.GetStatusCodeName(status));
      return false;
    }
  }
  else
  {
    // UL cards read 4 bytes at once -> 4 parts for 16 bytes
    for (byte part = 0; part < 4; part++)
    {
      status = mfrc522.MIFARE_Read(8 + part, buffer + 4 * part, &size);
      if (status != MFRC522::STATUS_OK) {
        Serial.print(F("MIFARE_Read() failed: "));
        Serial.println(mfrc522.GetStatusCodeName(status));
        return false;
      }
    }
  }

  Serial.print(F("Data on Card "));
  Serial.println(F(":"));
  dump_byte_array(buffer, 16);
  Serial.println();
  Serial.println();

  uint32_t tempCookie;
  tempCookie = (uint32_t)buffer[0] << 24;
  tempCookie += (uint32_t)buffer[1] << 16;
  tempCookie += (uint32_t)buffer[2] << 8;
  tempCookie += (uint32_t)buffer[3];

  nfcTag.cookie = tempCookie;
  nfcTag.version = buffer[4];
  nfcTag.nfcFolderSettings.folder = buffer[5];
  nfcTag.nfcFolderSettings.mode = buffer[6];
  nfcTag.nfcFolderSettings.special = buffer[7];
  nfcTag.nfcFolderSettings.special2 = buffer[8];

  myFolder = &nfcTag.nfcFolderSettings;

  return true;
}


void writeCard(const nfcTagObject& nfcTag) {
  byte buffer[16] = {0x13, 0x37, 0xb3, 0x47, // 0x1337 0xb347 magic cookie to
                     // identify our nfc tags
                     0x02,                   // version 1
                     nfcTag.nfcFolderSettings.folder,          // the folder picked by the user
                     nfcTag.nfcFolderSettings.mode,    // the playback mode picked by the user
                     nfcTag.nfcFolderSettings.special, // track or function for admin cards
                     nfcTag.nfcFolderSettings.special2,
                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                    };

  byte size = sizeof(buffer);

  MFRC522::PICC_Type mifareType = mfrc522.PICC_GetType(mfrc522.uid.sak);

  // Authenticate using key B
  //authentificate with the card and set card specific parameters
  if ((mifareType == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_1K ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_4K ) )
  {
    Serial.println(F("Authenticating again using key A..."));
    status = mfrc522.PCD_Authenticate(
               MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailerBlock, &key, &(mfrc522.uid));
  }
  else if (mifareType == MFRC522::PICC_TYPE_MIFARE_UL )
  {
    byte pACK[] = {0, 0}; //16 bit PassWord ACK returned by the NFCtag

    // Authenticate using key A
    Serial.println(F("Authenticating UL..."));
    status = mfrc522.PCD_NTAG216_AUTH(key.keyByte, pACK);
  }

  if (status != MFRC522::STATUS_OK) {
    Serial.print(F("PCD_Authenticate() failed: "));
    Serial.println(mfrc522.GetStatusCodeName(status));
    mp3.playMp3FolderTrack(401);
    return;
  }

  // Write data to the block
  Serial.print(F("Writing data into block "));
  Serial.print(blockAddr);
  Serial.println(F(" ..."));
  dump_byte_array(buffer, 16);
  Serial.println();

  if ((mifareType == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_1K ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_4K ) )
  {
    status = mfrc522.MIFARE_Write(blockAddr, buffer, 16);
  }
  else if (mifareType == MFRC522::PICC_TYPE_MIFARE_UL )
  {
    byte buffer2[16];
    byte size2 = sizeof(buffer2);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer, 4);
    status = mfrc522.MIFARE_Write(8, buffer2, 16);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer + 4, 4);
    status = mfrc522.MIFARE_Write(9, buffer2, 16);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer + 8, 4);
    status = mfrc522.MIFARE_Write(10, buffer2, 16);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer + 12, 4);
    status = mfrc522.MIFARE_Write(11, buffer2, 16);
  }

  if (status != MFRC522::STATUS_OK) {
    Serial.print(F("MIFARE_Write() failed: "));
    Serial.println(mfrc522.GetStatusCodeName(status));
    mp3.playMp3FolderTrack(401);
  }
  else
    mp3.playMp3FolderTrack(400);
  Serial.println();
  delay(100);
}



/**
   Helper routine to dump a byte array as hex values to Serial.
*/
void dump_byte_array(byte * buffer, byte bufferSize) {
  for (byte i = 0; i < bufferSize; i++) {
    Serial.print(buffer[i] < 0x10 ? " 0" : " ");
    Serial.print(buffer[i], HEX);
  }
}
