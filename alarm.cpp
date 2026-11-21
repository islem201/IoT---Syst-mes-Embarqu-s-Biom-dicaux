#include <WiFi.h>
#include <PubSubClient.h>

// =====================================
// CONFIGURATION WIFI & MQTT (Paramètres TP)
// =====================================
const char* ssid        = "iot";
const char* password    = "iotisis;";
const char* mqtt_server = "172.18.32.42";
const int   mqtt_port   = 1883;

// Topics MQTT de pilotage
const char* topic_cmd_alarme  = "esp32/alarme/commande"; // Reçoit "ON" depuis Node-RED
const char* topic_etat_alarme = "esp32/alarme/etat";     // Publie "ACTIVE" ou "ACQUITTEE"

WiFiClient espClient;
PubSubClient client(espClient);

// =====================================
// CONFIGURATION DES BROCHES - NOEUD DE PILOTAGE
// =====================================
#define LED_PIN     25  // LED sur IO25
#define BUTTON_PIN  26  // Bouton relié entre IO26 et GND
#define BUZZER_PIN  27  // Broche 'S' du module GT520 sur IO27

// =====================================
// VARIABLES D'ÉTAT DU SYSTÈME D'ALARME
// =====================================
bool alarmeActive = false; // Démarre en veille (attente d'une alerte Node-RED)
int dernierEtatBouton = HIGH;

unsigned long dernierBasculement = 0;
unsigned long lastMqttReconnect  = 0;
bool phaseAigue = false;
const unsigned long CADENCE_ALARME = 250; // Alternance toutes les 250 ms

// =====================================
// FONCTIONS D'ACTIVATION / ARRÊT DE L'ALARME
// =====================================
void demarrerAlarme() {
  if (!alarmeActive) {
    alarmeActive = true;
    dernierBasculement = 0;
    Serial.println("!!! [ALERTE NODE-RED] Declenchement LED + Sirene GT520 !!!");
    if (client.connected()) {
      client.publish(topic_etat_alarme, "ACTIVE");
    }
  }
}

void arreterAlarme() {
  if (alarmeActive) {
    alarmeActive = false;
    digitalWrite(LED_PIN, LOW);
    noTone(BUZZER_PIN);
    Serial.println(">>> [ACQUITTEMENT] Bouton appuye : Systeme d'alerte ARRETE !");
    if (client.connected()) {
      client.publish(topic_etat_alarme, "ACQUITTEE");
    }
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
  msgRecu.trim();

  Serial.print("[MQTT] Message recu sur ");
  Serial.print(topic);
  Serial.print(" : ");
  Serial.println(msgRecu);

  if (String(topic) == topic_cmd_alarme) {
    if (msgRecu == "ON") {
      demarrerAlarme();
    }
    // Note : conformément à la consigne, seul le bouton physique désactive l'alarme
  }
}

// =====================================
// CONNEXION WIFI & MQTT
// =====================================
void setup_wifi() {
  Serial.print("Connexion au WiFi ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("\nWiFi connecte ! IP : ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnect >= 5000) {
      lastMqttReconnect = now;
      Serial.print("Tentative connexion MQTT...");
      if (client.connect("ESP32Client_Pilotage_Alarme")) {
        Serial.println(" connecte !");
        client.subscribe(topic_cmd_alarme);
        client.publish(topic_etat_alarme, alarmeActive ? "ACTIVE" : "VEILLE");
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
  delay(300);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_PIN, LOW);

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  Serial.println("=========================================");
  Serial.println("ESP32 Noeud de Pilotage pret (en veille).");
  Serial.println("En attente d'alerte sur : esp32/alarme/commande");
  Serial.println("Appuyez sur le bouton (IO26) pour acquitter.");
  Serial.println("=========================================");
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

  // -------------------------------------
  // 1. DÉSACTIVATION UNIQUEMENT PAR BOUTON
  // -------------------------------------
  int etatActuelBouton = digitalRead(BUTTON_PIN);

  if (dernierEtatBouton == HIGH && etatActuelBouton == LOW) {
    delay(30); // Anti-rebond
    if (digitalRead(BUTTON_PIN) == LOW) {
      if (alarmeActive) {
        arreterAlarme();
      } else {
        Serial.println("[INFO] Bouton appuye (aucune alarme en cours).");
      }
    }
  }
  dernierEtatBouton = etatActuelBouton;

  // -------------------------------------
  // 2. SIRÈNE ET CLIGNOTEMENT SI ALARME ACTIVE
  // -------------------------------------
  if (alarmeActive) {
    if (maintenant - dernierBasculement >= CADENCE_ALARME) {
      dernierBasculement = maintenant;
      phaseAigue = !phaseAigue;

      if (phaseAigue) {
        digitalWrite(LED_PIN, HIGH);
        tone(BUZZER_PIN, 2200); // Ton aigu 2200 Hz
      } else {
        digitalWrite(LED_PIN, LOW);
        tone(BUZZER_PIN, 1400); // Ton grave 1400 Hz
      }
    }
  }
}
