#include <DFMiniMp3.h>
#include <EEPROM.h>
#include <JC_Button.h>
#include <MFRC522.h>
#include <SPI.h>
#include <SoftwareSerial.h>
#include <avr/sleep.h>

static const uint32_t cardCookie = 322417479;

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

static void nextTrack(uint16_t track);
uint8_t voiceMenu(int numberOfOptions, int startMessage, int messageOffset,
                  bool preview = false, int previewFromFolder = 0, int defaultValue = 0, bool exitWithLongPress = false);
bool isPlaying();
bool knownCard = false;

// implement a notification class,
// its member methods will get called
//
class Mp3Notify {
  public:
    static void OnError(uint16_t errorCode) {
      // see DfMp3_Error for code meaning
      Serial.println();
      Serial.print("Com Error ");
      Serial.println(errorCode);
    }
    static void OnPlayFinished(uint16_t track) {
      //      Serial.print("Track beendet");
      //      Serial.println(track);
      //      delay(100);
      nextTrack(track);
    }
    static void OnCardOnline(uint16_t code) {
      Serial.println(F("SD Karte online "));
    }
    static void OnCardInserted(uint16_t code) {
      Serial.println(F("SD Karte bereit "));
    }
    static void OnCardRemoved(uint16_t code) {
      Serial.println(F("SD Karte entfernt "));
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
  Serial.println(F("Queue :"));
  for (uint8_t x = 0; x < numTracksInFolder - firstTrack + 1 ; x++)
    Serial.println(queue[x]);

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

// Leider kann das Modul selbst keine Queue abspielen, daher müssen wir selbst die Queue verwalten
static uint16_t _lastTrackFinished;
static void nextTrack(uint16_t track) {
  if (track == _lastTrackFinished) {
    return;
  }
  Serial.println(F("=== nextTrack()"));
  _lastTrackFinished = track;

  if (!knownCard)
    // Wenn eine neue Karte angelernt wird soll das Ende eines Tracks nicht
    // verarbeitet werden
    return;

  switch (myFolder->mode)
  {    
  case Hoerspiel:
  case HoerspielRandom:
    Serial.println(F("Hörspielmodus ist aktiv -> keinen neuen Track spielen"));
    setstandbyTimer();
  break;
  
  case Album:
  case SpezialVonBis:
    if (currentTrack != numTracksInFolder) {
      currentTrack++;
      mp3.playFolderTrack(myFolder->folder, currentTrack);
      Serial.print(F("Albummodus ist aktiv -> nächster Track: "));
      Serial.print(currentTrack);
    } else
      setstandbyTimer();
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
    setstandbyTimer();
    break;
  
  case Hoerbuch:
    if (currentTrack != numTracksInFolder) {
      currentTrack = currentTrack + 1;
      Serial.print(F("Hörbuch Modus ist aktiv -> nächster Track und Fortschritt speichern"));
      Serial.println(currentTrack);
      mp3.playFolderTrack(myFolder->folder, currentTrack);
      // Fortschritt im EEPROM abspeichern
      EEPROM.update(myFolder->folder, currentTrack);
    } else {
      // Fortschritt zurück setzen
      EEPROM.update(myFolder->folder, 1);
      setstandbyTimer();
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

#define pinButtonPrev A0
#define pinButtonNext A1
#define pinButtonVolP A2
#define pinButtonVolM A3
#define busyPin 4
#define shutdownPin 7

#define LONG_PRESS 1000


/// Funktionen für den Standby Timer (z.B. über Pololu-Switch oder Mosfet)

void setstandbyTimer() {
  Serial.println(F("=== setstandbyTimer()"));
  if (mySettings.standbyTimer != 0)
    sleepAtMillis = millis() + (mySettings.standbyTimer * 60 * 1000);
  else
    sleepAtMillis = 0;
  Serial.println(sleepAtMillis);
}

void disablestandbyTimer() {
  Serial.println(F("=== disablestandby()"));
  sleepAtMillis = 0;
}

void powerOff()
{
    Serial.println(F("=== power off!"));
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
    ExtButton(uint8_t pin) : Button(pin), longPressCount(1) {};

    byte readEvent()
    {
        read();
		if (wasPressed())
		{
			longPressCount = 0;
		}
        if (wasReleased())
        {
            bool bShort = (longPressCount == 0);
            longPressCount = 1;	// use 1 as marker to not send event
            if (bShort) return BTN_SHORT_PRESS;
        }
		if (pressedFor(static_cast<uint16_t>(longPressCount + 1) * LONG_PRESS))
        {
            return (longPressCount++ == 0) ? BTN_LONG_PRESS : BTN_LONG_REPEAT;
        }
		return BTN_NO_PRESS;
	}
	
private:
    uint8_t    longPressCount;
};


ExtButton buttonVolP(pinButtonVolP);
ExtButton buttonVolM(pinButtonVolM);
ExtButton buttonPrev(pinButtonPrev);
ExtButton buttonNext(pinButtonNext);

bool isPlaying() {
  return !digitalRead(busyPin);
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
  randomSeed(analogRead(A7)); // Zufallsgenerator initialisieren

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

  // DFPlayer Mini initialisieren
  mp3.begin();
  // Zwei Sekunden warten bis der DFPlayer Mini initialisiert ist
  delay(2000);
  volume = mySettings.initVolume;
  mp3.setVolume(volume);
  mp3.setEq(mySettings.eq - 1);
  // Fix für das Problem mit dem Timeout (ist jetzt in Upstream daher nicht mehr nötig!)
  //mySoftwareSerial.setTimeout(10000);

  // NFC Leser initialisieren
  SPI.begin();        // Init SPI bus
  mfrc522.PCD_Init(); // Init MFRC522
  mfrc522.PCD_DumpVersionToSerial(); // Show details of PCD - MFRC522 Card Reader
  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

  pinMode(pinButtonVolP, INPUT_PULLUP);
  pinMode(pinButtonVolM, INPUT_PULLUP);
  pinMode(pinButtonPrev, INPUT_PULLUP);
  pinMode(pinButtonNext, INPUT_PULLUP);
  pinMode(shutdownPin, OUTPUT);
  digitalWrite(shutdownPin, LOW);

  // RESET --- ALLE DREI KNÖPFE BEIM STARTEN GEDRÜCKT HALTEN -> alle EINSTELLUNGEN werden gelöscht
  if (digitalRead(pinButtonVolP) == LOW && digitalRead(pinButtonVolM) == LOW &&
      digitalRead(pinButtonPrev) == LOW && digitalRead(pinButtonNext) == LOW) {
    Serial.println(F("Reset -> EEPROM wird gelöscht"));
    for (int i = 0; i < EEPROM.length(); i++) {
      EEPROM.update(i, 0);
    }
  }
  // Start Shortcut "at Startup" - e.g. Welcome Sound
  playShortCut(3);
}

byte btnEvVolP = 0;
byte btnEvVolM = 0;
byte btnEvPrev = 0;
byte btnEvNext = 0;

void readButtons() {
  btnEvVolP = buttonVolP.readEvent();
  btnEvVolM = buttonVolM.readEvent();
  btnEvPrev = buttonPrev.readEvent();
  btnEvNext = buttonNext.readEvent();
}

void volumeUpButton() {
  Serial.println(F("=== volumeUp()"));
  if (volume < mySettings.maxVolume)
    mp3.setVolume(++volume);
  
  Serial.println(volume);
}

void volumeDownButton() {
  Serial.println(F("=== volumeDown()"));
  if (volume > mySettings.minVolume)
    mp3.setVolume(--volume);

  Serial.println(volume);
}

void nextButton() {
  nextTrack(random(65536));
  delay(1000);
}

void previousButton() {
  previousTrack();
  delay(1000);
}

void playFolder() {
  disablestandbyTimer();
  randomSeed(millis() + random(1000));
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
  Serial.println(F("=== playShortCut()"));
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

void loop() {

  checkStandbyAtMillis();
  mp3.loop();
  readButtons();
  
  // admin menu
  if ((buttonVolP.pressedFor(LONG_PRESS) || buttonPrev.pressedFor(LONG_PRESS)) && 
       buttonVolP.isPressed() && buttonPrev.isPressed())
  {
    mp3.pause();
    do {
      readButtons();
    } while (buttonVolP.isPressed() || buttonPrev.isPressed());
    readButtons();
    adminMenu();
    return;
  }
  
  // playAdvertisement only works when regular track is playing already!
  switch (btnEvVolP)
  {
  case BTN_SHORT_PRESS: Serial.println(F("Vol+ short")); volumeUpButton();   break;
  case BTN_LONG_PRESS:  Serial.println(F("Vol+ long"));                      break;
  }
  switch (btnEvVolM)
  {
  case BTN_SHORT_PRESS: Serial.println(F("Vol- short")); volumeDownButton(); break;
  case BTN_LONG_PRESS:  Serial.println(F("Vol- long"));  powerOff();         break;
  }
  switch (btnEvPrev)
  {
  case BTN_SHORT_PRESS: Serial.println(F("Prev short")); previousButton();   break;
  case BTN_LONG_PRESS:  Serial.println(F("Prev long")); mp3.playAdvertisement(2); break;
  }
  switch (btnEvNext)
  {
  case BTN_SHORT_PRESS: Serial.println(F("Next short")); nextButton();       break;
  case BTN_LONG_PRESS:  Serial.println(F("Next long"));                      break;
  }
  // Ende der Buttons
  
  if (mfrc522.PICC_IsNewCardPresent())  // RFID Karte wurde aufgelegt
  {
    if (mfrc522.PICC_ReadCardSerial())
    {
      if (readCard(&myCard))
      {
        if (myCard.cookie == cardCookie && myFolder->folder != 0 && myFolder->mode != Uninitialized) {
          randomSeed(millis()); // make random a little bit more "random"
          playFolder();
        }
        else { 
		  // Neue Karte konfigurieren
          knownCard = false;
          setupCard();
        }
      }
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
    }
  }  
}

void adminMenu() {
  disablestandbyTimer();
  mp3.pause();
  Serial.println(F("=== adminMenu()"));
  knownCard = false;

  int subMenu = voiceMenu(10, 900, 900, false, false, 0, true);
  if (subMenu == 0)
    return;
  if (subMenu == 1) {
    resetCard();
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
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
    mp3.setEq(mySettings.eq - 1);
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
      const byte aStandbyTimer[] = { 5, 15, 30, 60, 0};
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
        if (btnEvPrev == BTN_SHORT_PRESS || btnEvNext == BTN_SHORT_PRESS)
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
    if (temp == 2) {
      mySettings.invertVolumeButtons = true;
    }
    else {
      mySettings.invertVolumeButtons = false;
    }
  }
  writeSettingsToFlash();
  setstandbyTimer();
}

uint8_t voiceMenu(int numberOfOptions, int startMessage, int messageOffset,
                  bool preview = false, int previewFromFolder = 0, int defaultValue = 0, bool exitWithLongPress = false) {
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

	switch (btnEvPrev)
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
	
	switch (btnEvNext)
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
	
	switch (btnEvVolP)
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
  mp3.playMp3FolderTrack(800);
  do {
	  readButtons();
      if (btnEvPrev == BTN_SHORT_PRESS || btnEvNext == BTN_SHORT_PRESS) {
      Serial.print(F("Abgebrochen!"));
      mp3.playMp3FolderTrack(802);
      return;
    }
  } while (!mfrc522.PICC_IsNewCardPresent());

  if (!mfrc522.PICC_ReadCardSerial())
    return;

  Serial.print(F("Karte wird neu Konfiguriert!"));
  setupCard();
}

void setupFolder(folderSettings * theFolder) {
  // Ordner abfragen
  theFolder->folder = voiceMenu(99, 301, 0, true);

  // Wiedergabemodus abfragen
  theFolder->mode = voiceMenu(9, 310, 310);

  //  // Hörbuchmodus -> Fortschritt im EEPROM auf 1 setzen
  //  EEPROM.update(theFolder->folder, 1);

  switch (theFolder->mode)
  {
  // Einzelmodus -> Datei abfragen
  case EinzelTitel:
    theFolder->special = voiceMenu(mp3.getFolderTrackCount(theFolder->folder), 320, 0,
                                   true, theFolder->folder);
    break;
  // Admin Funktionen
  case Admin:
    theFolder->special = voiceMenu(3, 320, 320);
  break;

  // Spezialmodus Von-Bis
  case HoerspielRandom:
  case SpezialVonBis:
  case PartyRandom:
  theFolder->special = voiceMenu(mp3.getFolderTrackCount(theFolder->folder), 321, 0,
                                   true, theFolder->folder);
    theFolder->special2 = voiceMenu(mp3.getFolderTrackCount(theFolder->folder), 322, 0,
                                    true, theFolder->folder, theFolder->special);
  break;
  }
}

void setupCard() {
  mp3.pause();
  Serial.println(F("=== setupCard()"));
  setupFolder(&myCard.nfcFolderSettings);
  // Karte ist konfiguriert -> speichern
  mp3.pause();
  while (isPlaying());
  writeCard(myCard);
}

bool readCard(nfcTagObject * nfcTag) {
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

  nfcTag->cookie = tempCookie;
  nfcTag->version = buffer[4];
  nfcTag->nfcFolderSettings.folder = buffer[5];
  nfcTag->nfcFolderSettings.mode = buffer[6];
  nfcTag->nfcFolderSettings.special = buffer[7];
  nfcTag->nfcFolderSettings.special2 = buffer[8];

  myFolder = &nfcTag->nfcFolderSettings;

  return true;
}

void writeCard(nfcTagObject nfcTag) {
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
    Serial.println(F("Authenticating again using key B..."));
    status = mfrc522.PCD_Authenticate(
               MFRC522::PICC_CMD_MF_AUTH_KEY_B, trailerBlock, &key, &(mfrc522.uid));
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
