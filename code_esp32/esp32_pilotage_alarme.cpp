#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

// =====================================
// CONFIGURATION WIFI & MQTT (Paramètres TP)
// =====================================
const char* ssid        = "iot";
const char* password    = "iotisis;";
const char* mqtt_server = "172.18.32.42";
const int   mqtt_port   = 1883;

// Topics MQTT alignés avec le flux Node-RED "Monitoring Santé & Alarme"
const char* topic_cmd_in = "esp32/alarme/commande"; // Reçoit "ON" / "OFF" depuis Node-RED
const char* topic_etat   = "esp32/alarme/etat";     // Publie VEILLE / ACTIVE / ACQUITTEE / APPUI_BOUTON / HORS_LIGNE

WiFiClient espClient;
PubSubClient client(espClient);

// =====================================
// CONFIGURATION DES BROCHES
// =====================================
#define LED_PIN     25  // LED d'alerte sur IO25 (avec résistance 220 Ohms)
#define BUTTON_PIN  26  // Bouton poussoir entre IO26 et GND (INPUT_PULLUP)
#define BUZZER_PIN  27  // Broche 'S' du module buzzer passif GT520 sur IO27

// =====================================
// PARAMÈTRES DE TEMPS
// =====================================
const unsigned long CADENCE_ALARME    = 250;   // Alternance sirène / LED (250 ms)
const unsigned long DEBOUNCE_MS       = 40;    // Anti-rebond matériel du bouton
const unsigned long DOUBLE_MS         = 700;   // Fenêtre de double appui (identique à fn_button Node-RED)
const unsigned long IGNORE_ON_MS      = 1500;  // Délai de grâce après acquittement local
const unsigned long AFFICHE_ACQUIT_MS = 8000;  // Durée d'affichage de "ACQUITTEE" avant retour à "VEILLE"

// =====================================
// VARIABLES D'ÉTAT
// =====================================
bool alarmeActive = false;
bool phaseAigue = false;
unsigned long dernierBasculement = 0;
unsigned long lastMqttReconnect  = 0;

// Gestion bouton (simple / double appui)
int lectureBrute = HIGH;
int etatStable   = HIGH;
unsigned long tChangement         = 0;
unsigned long dernierAppui        = 0;
unsigned long dernierAcquittement = 0;
bool afficheAcquittee             = false;

// =====================================
// UTILITAIRES MQTT
// =====================================
void publierEtat(const char* etat, bool retain = true) {
  if (client.connected()) {
    client.publish(topic_etat, etat, retain);
  }
}

// =====================================
// ACTIVATION / ARRÊT DE L'ALARME
// =====================================
void demarrerAlarme() {
  // Ignore un ordre ON résiduel qui arriverait juste après un appui physique
  if (dernierAcquittement != 0 && millis() - dernierAcquittement < IGNORE_ON_MS) {
    Serial.println("[ALARME] Ordre ON ignore (acquittement physique recent)");
    return;
  }
  if (!alarmeActive) {
    alarmeActive = true;
    afficheAcquittee = false;
    dernierBasculement = 0;
    Serial.println("!!! [ALERTE] Declenchement LED (IO25) + Sirene GT520 (IO27) !!!");
    publierEtat("ACTIVE", true);
  }
}

void arreterAlarme(const char* nouvelEtat) {
  if (alarmeActive) {
    alarmeActive = false;
    digitalWrite(LED_PIN, LOW);
    noTone(BUZZER_PIN);
    Serial.print(">>> [ALARME] Arretee -> ");
    Serial.println(nouvelEtat);
    publierEtat(nouvelEtat, true);
  }
}

// =====================================
// RÉCEPTION DES ORDRES MQTT DEPUIS NODE-RED
// =====================================
void callback(char* topic, byte* message, unsigned int length) {
  String msgRecu = "";
  for (unsigned int i = 0; i < length; i++) {
    msgRecu += (char)message[i];
  }
  msgRecu.replace("\"", "");
  msgRecu.trim();
  msgRecu.toUpperCase();

  Serial.print("[MQTT RECU] ");
  Serial.print(topic);
  Serial.print(" = ");
  Serial.println(msgRecu);

  if (strcmp(topic, topic_cmd_in) == 0) {
    if (msgRecu == "ON") {
      demarrerAlarme();
    } else if (msgRecu == "OFF") {
      // Déclenché par le bouton "Couper l'alarme" du Dashboard, la fin du test 5s ou le retour à la normale
      arreterAlarme("VEILLE");
    }
  }
}

// =====================================
// GESTION DU BOUTON : 1 APPUI = SILENCE / 2 APPUIS = REC CSV
// =====================================
void gererAppui(unsigned long now) {
  // 1. Notifie immédiatement Node-RED sur esp32/alarme/etat (correspond à PRESS_REGEX dans fn_button)
  // - Au 1er appui : Node-RED reçoit "APPUI_BOUTON" -> déclenche {topic: "silence"}
  // - Au 2e appui (< 700 ms) : Node-RED reçoit "APPUI_BOUTON" -> déclenche {topic: "toggle"} (REC CSV)
  publierEtat("APPUI_BOUTON", false);

  if (dernierAppui != 0 && (now - dernierAppui) <= DOUBLE_MS) {
    // 2e appui rapide détecté
    dernierAppui = 0;
    Serial.println("[BOUTON] DOUBLE appui (< 700 ms) -> Bascule enregistrement CSV sur Node-RED");
    publierEtat(alarmeActive ? "ACTIVE" : (afficheAcquittee ? "ACQUITTEE" : "VEILLE"), true);
  } else {
    // 1er appui : coupure matérielle immédiate de l'alarme (même si le Wi-Fi est coupé)
    dernierAppui = now;
    Serial.println("[BOUTON] Appui simple -> Coupure alarme");
    if (alarmeActive) {
      arreterAlarme("ACQUITTEE");
      dernierAcquittement = now;
      afficheAcquittee = true;
    } else {
      // Restaure l'affichage de statut retenu sur la carte Dashboard
      publierEtat(afficheAcquittee ? "ACQUITTEE" : "VEILLE", true);
    }
  }
}

void lireBouton() {
  unsigned long now = millis();
  int r = digitalRead(BUTTON_PIN);

  if (r != lectureBrute) {
    lectureBrute = r;
    tChangement = now;
  }

  if ((now - tChangement) > DEBOUNCE_MS && r != etatStable) {
    etatStable = r;
    if (etatStable == LOW) { // Front descendant = appui physique
      gererAppui(now);
    }
  }
}

// =====================================
// CONNEXION WIFI & MQTT (NON-BLOQUANTE)
// =====================================
void setup_wifi() {
  Serial.print("Connexion au WiFi ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  unsigned long debut = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - debut < 8000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nWiFi connecte ! IP : ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi non disponible au demarrage (tentative en arriere-plan)");
  }
}

void reconnect() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnect >= 3000) {
      lastMqttReconnect = now;
      Serial.print("Tentative connexion MQTT...");

      String clientId = "ESP32_Pilotage_" + String((uint32_t)ESP.getEfuseMac(), HEX);
      // Last Will : publie "HORS_LIGNE" si l'ESP32 perd le courant ou le réseau
      if (client.connect(clientId.c_str(), NULL, NULL, topic_etat, 1, true, "HORS_LIGNE")) {
        Serial.println(" connecte !");
        client.subscribe(topic_cmd_in, 1);
        publierEtat(alarmeActive ? "ACTIVE" : "VEILLE", true);
      } else {
        Serial.print(" echec, rc=");
        Serial.println(client.state());
      }
    }
  }
}

// =====================================
// INITIALISATION
// =====================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  Serial.println("===================================================");
  Serial.println("ESP32 Noeud de Pilotage pret (Synchronise Node-RED)");
  Serial.println("  - Ecoute ordres   : esp32/alarme/commande (ON/OFF)");
  Serial.println("  - Etat & Bouton   : esp32/alarme/etat");
  Serial.println("  - 1 appui         : Couper l'alarme (Silence)");
  Serial.println("  - 2 appuis (<700ms): Demarrer/Arreter CSV");
  Serial.println("===================================================");
}

// =====================================
// BOUCLE PRINCIPALE
// =====================================
void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  unsigned long maintenant = millis();

  // 1. Lecture continue du bouton physique (anti-rebond sans delay)
  lireBouton();

  // 2. Retour automatique de ACQUITTEE vers VEILLE après 8 secondes
  if (afficheAcquittee && !alarmeActive && (maintenant - dernierAcquittement > AFFICHE_ACQUIT_MS)) {
    afficheAcquittee = false;
    publierEtat("VEILLE", true);
  }

  // 3. Sirène modulée (2200 Hz / 1400 Hz) + LED clignotante
  if (alarmeActive) {
    if (maintenant - dernierBasculement >= CADENCE_ALARME) {
      dernierBasculement = maintenant;
      phaseAigue = !phaseAigue;

      if (phaseAigue) {
        digitalWrite(LED_PIN, HIGH);
        tone(BUZZER_PIN, 2200);
      } else {
        digitalWrite(LED_PIN, LOW);
        tone(BUZZER_PIN, 1400);
      }
    }
  }
}
