#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// ─────────────────────────────────────────────────────────────
//  UUIDs Bluetooth (ne jamais changer — l'app les connaît déjà)
// ─────────────────────────────────────────────────────────────
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_UUID     "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define EVT_UUID     "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"

// ─────────────────────────────────────────────────────────────
//  Brochage DFPlayer
//  D5 = RX Arduino  (reçoit les données du DFPlayer)
//  D4 = TX Arduino  (envoie vers DFPlayer — résistance 1kΩ obligatoire)
//  Alimentation DFPlayer : 5V sur VBUS, jamais sur 3.3V
// ─────────────────────────────────────────────────────────────
#define BROCHE_RX  D5
#define BROCHE_TX  D4

// ─────────────────────────────────────────────────────────────
//  Fichiers audio sur la carte SD  (/mp3/0001.mp3, etc.)
// ─────────────────────────────────────────────────────────────
#define SON_DEMARRAGE   1   // joué au démarrage de l'ESP32
#define SON_CONNEXION   2   // joué quand le téléphone se connecte
#define SON_BIP_COURSE  3   // le bip de départ
#define SON_TEST        4   // test depuis les paramètres de l'app

// ─────────────────────────────────────────────────────────────
//  Volumes
// ─────────────────────────────────────────────────────────────
#define VOLUME_SYSTEME  10   // sons démarrage / connexion : discrets
#define VOLUME_DEFAUT   25   // bip de course : réglable depuis l'app

// ─────────────────────────────────────────────────────────────
//  Machine à états de la séquence de départ
// ─────────────────────────────────────────────────────────────
enum Etat { IDLE, CONCENTRATION, DELAI_ALEA, TERMINE };
volatile Etat etatActuel = IDLE;

unsigned long debutPhase = 0;
unsigned long dureeConc  = 3000;
unsigned long delaiMin   = 1000;
unsigned long delaiMax   = 3000;
unsigned long delaiTire  = 0;

// ─────────────────────────────────────────────────────────────
//  Objets Bluetooth
// ─────────────────────────────────────────────────────────────
BLEServer*         serveurBLE  = nullptr;
BLECharacteristic* caracterEVT = nullptr;
bool telephoneConnecte = false;

// ─────────────────────────────────────────────────────────────
//  Objet DFPlayer
// ─────────────────────────────────────────────────────────────
HardwareSerial      serialDF(1);   // UART1 du ESP32-S3
DFRobotDFPlayerMini dfplayer;
bool moduleSON    = false;
int  volumeCourse = VOLUME_DEFAUT;

// ─────────────────────────────────────────────────────────────
//  File d'attente pour les sons
//  Le DFPlayer ne supporte pas deux commandes dos-à-dos.
//  On met les sons en file et on les envoie avec l'espacement requis.
// ─────────────────────────────────────────────────────────────
struct CommandeSon { int piste; int volume; };
CommandeSon fileSons[4];
int  fileTete  = 0;
int  fileQueue = 0;
unsigned long derniereSon      = 0;
const unsigned long DELAI_SON  = 350;   // ms minimum entre deux commandes

void ajouterSon(int piste, int volume) {
  int suivant = (fileQueue + 1) % 4;
  if (suivant == fileTete) return;  // file pleine, on abandonne
  fileSons[fileQueue] = {piste, volume};
  fileQueue = suivant;
}

void traiterFileSons() {
  if (fileTete == fileQueue) return;                    // rien à jouer
  if (!moduleSON) { fileTete = fileQueue; return; }     // pas de module
  if (millis() - derniereSon < DELAI_SON) return;       // trop tôt

  CommandeSon c = fileSons[fileTete];
  fileTete = (fileTete + 1) % 4;

  dfplayer.volume(c.volume);
  delay(20);              // délai OBLIGATOIRE entre volume() et play()
  dfplayer.play(c.piste);
  derniereSon = millis();
}

void jouerSonSysteme(int piste) { ajouterSon(piste, VOLUME_SYSTEME); }
void jouerSonCourse(int piste)  { ajouterSon(piste, volumeCourse);   }

// ─────────────────────────────────────────────────────────────
//  Communication Bluetooth
// ─────────────────────────────────────────────────────────────
void envoyerMessage(const String& msg) {
  if (!telephoneConnecte || !caracterEVT) return;
  caracterEVT->setValue(msg.c_str());
  caracterEVT->notify();
  Serial.println(">> " + msg);
}

// ─────────────────────────────────────────────────────────────
//  Traitement des commandes reçues depuis l'application
// ─────────────────────────────────────────────────────────────
void traiterCommande(const String& brut) {
  String cmd = brut;
  cmd.trim();
  Serial.println("<< " + cmd);

  // START:dureeConc:delaiMin:delaiMax  (millisecondes)
  if (cmd.startsWith("START:")) {
    if (etatActuel != IDLE) { envoyerMessage("ERR:BUSY"); return; }

    int a = cmd.indexOf(':', 6);
    int b = (a > 0) ? cmd.indexOf(':', a + 1) : -1;

    if (a > 0 && b > 0) {
      dureeConc = (unsigned long)cmd.substring(6, a).toInt();
      delaiMin  = (unsigned long)cmd.substring(a + 1, b).toInt();
      delaiMax  = (unsigned long)cmd.substring(b + 1).toInt();
    } else {
      dureeConc = 3000; delaiMin = 1000; delaiMax = 3000;
    }

    // Valeurs minimales de sécurité
    if (dureeConc < 500)       dureeConc = 500;
    if (delaiMin  < 300)       delaiMin  = 300;
    if (delaiMax  <= delaiMin) delaiMax  = delaiMin + 500;

    // Tirage du délai aléatoire dès maintenant (imprévisible)
    delaiTire  = (unsigned long)random((long)delaiMin, (long)delaiMax + 1);
    debutPhase = millis();
    etatActuel = CONCENTRATION;

    envoyerMessage("PHASE:WAIT");
    Serial.println("[SEQ] conc=" + String(dureeConc) + "ms delai=" + String(delaiTire) + "ms");
    return;
  }

  // VOL:0-30
  if (cmd.startsWith("VOL:")) {
    volumeCourse = constrain(cmd.substring(4).toInt(), 0, 30);
    envoyerMessage("VOL_OK:" + String(volumeCourse));
    return;
  }

  // ABORT  — faux départ ou annulation
  if (cmd == "ABORT") {
    etatActuel = IDLE;
    envoyerMessage("ABORTED");
    return;
  }

  // FINISH — session terminée, retour en attente
  if (cmd == "FINISH") {
    etatActuel = IDLE;
    envoyerMessage("FINISH_ACK");
    return;
  }

  // TEST_BIP — test du son depuis les paramètres
  if (cmd == "TEST_BIP") {
    if (etatActuel != IDLE) { envoyerMessage("ERR:BUSY"); return; }
    jouerSonCourse(SON_TEST);
    envoyerMessage("TEST_BIP_OK");
    return;
  }

  // PING — vérification de connexion
  if (cmd == "PING") {
    envoyerMessage("PONG:vol=" + String(volumeCourse) + ":son=" + String(moduleSON ? 1 : 0));
    return;
  }
}

// ─────────────────────────────────────────────────────────────
//  Callbacks Bluetooth
// ─────────────────────────────────────────────────────────────
class GestionnaireConnexion : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    telephoneConnecte = true;
    Serial.println("[BLE] telephone connecte");
    delay(200);  // laisser la connexion se stabiliser avant d'envoyer
    envoyerMessage("CONNECTED:vol=" + String(volumeCourse));
    jouerSonSysteme(SON_CONNEXION);
  }
  void onDisconnect(BLEServer* s) override {
    telephoneConnecte = false;
    etatActuel = IDLE;
    Serial.println("[BLE] deconnecte — relance publicite...");
    delay(300);
    s->startAdvertising();  // redevient visible pour une reconnexion
  }
};

class GestionnaireCommandes : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    traiterCommande(String(c->getValue().c_str()));
  }
};

// ─────────────────────────────────────────────────────────────
//  Initialisation du DFPlayer
// ─────────────────────────────────────────────────────────────
bool demarrerDFPlayer() {
  Serial.print("[SON] initialisation DFPlayer... ");

  // UART1 : D5=RX Arduino (DFPlayer TX), D4=TX Arduino (DFPlayer RX + 1kΩ)
  serialDF.begin(9600, SERIAL_8N1, BROCHE_RX, BROCHE_TX);

  // Le DFPlayer met du temps à lire la carte SD au démarrage
  // 1500ms minimum — en dessous, begin() renvoie false
  delay(1500);

  if (!dfplayer.begin(serialDF, true, true)) {
    Serial.println("ECHEC");
    Serial.println("  verifier : cablage, carte SD FAT32, dossier /mp3");
    Serial.println("  fichiers : 0001.mp3 / 0002.mp3 / 0003.mp3 / 0004.mp3");
    return false;
  }

  dfplayer.setTimeOut(500);
  dfplayer.EQ(DFPLAYER_EQ_NORMAL);
  dfplayer.outputDevice(DFPLAYER_DEVICE_SD);
  dfplayer.volume(VOLUME_SYSTEME);
  delay(200);

  int nbFichiers = dfplayer.readFileCounts(DFPLAYER_DEVICE_SD);
  Serial.println("OK — " + String(nbFichiers) + " fichier(s)");

  if (nbFichiers <= 0) {
    Serial.println("  ATTENTION : aucun fichier detecte sur la carte SD");
    Serial.println("  verifier que les .mp3 sont dans /mp3/ (pas /mp3/mp3/)");
  }

  return true;
}

// =============================================================
//  SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== StartGo ===");

  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, HIGH);  // LED éteinte (active LOW)

  // Graine aléatoire — pour que le délai soit vraiment imprévisible
  randomSeed(analogRead(A0) ^ (unsigned long)micros());

  // Démarrage du module son
  moduleSON = demarrerDFPlayer();
  if (moduleSON) {
    // Son de démarrage joué directement (hors file, une seule fois)
    dfplayer.volume(VOLUME_SYSTEME);
    delay(20);
    dfplayer.play(SON_DEMARRAGE);
    derniereSon = millis();
    Serial.println("[SON] son demarrage lance");
  }

  // Initialisation Bluetooth
  Serial.print("[BLE] initialisation... ");
  BLEDevice::init("StartGo-Pro");
  serveurBLE = BLEDevice::createServer();
  serveurBLE->setCallbacks(new GestionnaireConnexion());

  BLEService* service = serveurBLE->createService(SERVICE_UUID);

  // Caractéristique CMD : l'app envoie des commandes ici
  BLECharacteristic* caracterCMD = service->createCharacteristic(
    CMD_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  caracterCMD->setCallbacks(new GestionnaireCommandes());

  // Caractéristique EVT : l'ESP32 envoie des événements ici
  caracterEVT = service->createCharacteristic(
    EVT_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  caracterEVT->addDescriptor(new BLE2902());

  service->start();

  BLEAdvertising* pub = BLEDevice::getAdvertising();
  pub->addServiceUUID(SERVICE_UUID);
  pub->setScanResponse(true);
  pub->setMinPreferred(0x06);
  BLEDevice::startAdvertising();
  Serial.println("ok — visible sous le nom StartGo-Pro");

  // 5 clignotements : tout est prêt
  for (int i = 0; i < 5; i++) {
    digitalWrite(LED_RED, LOW);  delay(80);
    digitalWrite(LED_RED, HIGH); delay(80);
  }

  Serial.println("[PRET] en attente de connexion Bluetooth");
}

// =============================================================
//  LOOP
// =============================================================
void loop() {
  unsigned long maintenant = millis();

  // Sons en attente → on les envoie au bon moment
  traiterFileSons();

  // Machine à états de la séquence de départ
  switch (etatActuel) {

    case CONCENTRATION:
      if (maintenant - debutPhase >= dureeConc) {
        debutPhase = maintenant;
        etatActuel = DELAI_ALEA;
        envoyerMessage("PHASE:ALEA");
        Serial.println("[SEQ] phase aleatoire — " + String(delaiTire) + "ms");
      }
      break;

    case DELAI_ALEA:
      if (maintenant - debutPhase >= delaiTire) {
        etatActuel = TERMINE;
        jouerSonCourse(SON_BIP_COURSE);
        envoyerMessage("BIP:" + String(maintenant));
        Serial.println("[SEQ] BIP !");
        // Flash LED visible sur la carte
        digitalWrite(LED_RED, LOW);  delay(100);
        digitalWrite(LED_RED, HIGH);
      }
      break;

    case TERMINE:
      // Attente du FINISH ou ABORT depuis l'app
      break;

    default:
      break;
  }

  delay(5);
}
