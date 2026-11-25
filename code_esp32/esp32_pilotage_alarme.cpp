#include <WiFi.h>
#include <PubSubClient.h>

// =====================================
// CONFIGURATION WIFI & MQTT (Paramètres TP)
// =====================================
const char* ssid        = "iot";
const char* password    = "iotisis;";
const char* mqtt_server = "172.18.32.42";
const int   mqtt_port   = 1883;

// Topics MQTT
const char* topic_cmd_in      = "esp32/alarme/commande"; // Reçoit "ON" depuis Node-RED
const char* topic_btn_command = "esp32/alarme/command";  // Publie "on" (appuyé) ou "off" (relâché)
const char* topic_etat_alarme = "esp32/alarme/etat";     // Publie "ACTIVE" ou "ACQUITTEE"

WiFiClient espClient;
PubSubClient client(espClient);

// =====================================
// CONFIGURATION DES BROCHES
// =====================================
#define LED_PIN     25  // LED sur IO25
#define BUTTON_PIN  26  // Bouton relié entre IO26 et GND
#define BUZZER_PIN  27  // Broche 'S' du module GT520 sur IO27

// =====================================
// VARIABLES D'ÉTAT DU SYSTÈME D'ALARME
// =====================================
bool alarmeActive = false;
int dernierEtatBouton = HIGH;

unsigned long dernierBasculement = 0;
unsigned long lastMqttReconnect  = 0;
bool phaseAigue = false;
const unsigned long CADENCE_ALARME = 250;

// =====================================
// FONCTIONS D'ACTIVATION / ARRÊT DE L'ALARME
// =====================================
void demarrerAlarme() {
  if (!alarmeActive) {
    alarmeActive = true;
    dernierBasculement = 0;
    Serial.println("!!! [ALERTE] Declenchement LED + Sirene GT520 !!!");
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
    Serial.println(">>> [ACQUITTEMENT] Alarme ARRETEE par bouton !");
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
  msgRecu.toUpperCase(); // Accepte "on", "ON", "\"ON\""

  Serial.print("[MQTT RECU] Topic: ");
  Serial.print(topic);
  Serial.print(" | Payload: ");
  Serial.println(msgRecu);

  if (String(topic) == topic_cmd_in) {
    if (msgRecu.indexOf("ON") != -1) {
      demarrerAlarme();
    }
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
    if (now - lastMqttReconnect >= 3000) {
      lastMqttReconnect = now;
      Serial.print("Tentative connexion MQTT...");
      
      // ClientID unique pour éviter toute déconnexion mutuelle entre les ESP32
      String clientId = "ESP32_Pilotage_" + String((uint32_t)ESP.getEfuseMac(), HEX);
      if (client.connect(clientId.c_str())) {
        Serial.println(" connecte !");
        client.subscribe(topic_cmd_in);
        client.publish(topic_etat_alarme, alarmeActive ? "ACTIVE" : "VEILLE");
        client.publish(topic_btn_command, "off");
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
  Serial.println("ESP32 Noeud de Pilotage pret.");
  Serial.println("Ecoute ordre alarme : esp32/alarme/commande");
  Serial.println("Publie etat bouton  : esp32/alarme/command (on/off)");
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
  // 1. GESTION DU BOUTON PHYSIQUE (on / off + arrêt alarme)
  // -------------------------------------
  int etatActuelBouton = digitalRead(BUTTON_PIN);

  if (etatActuelBouton != dernierEtatBouton) {
    delay(30); // Anti-rebond
    etatActuelBouton = digitalRead(BUTTON_PIN);

    if (etatActuelBouton != dernierEtatBouton) {
      dernierEtatBouton = etatActuelBouton;

      if (etatActuelBouton == LOW) {
        // Bouton APPUYÉ -> coupe l'alarme et publie command = "on"
        Serial.println("[BOUTON] Appuye -> command = on");
        if (client.connected()) {
          client.publish(topic_btn_command, "on");
        }
        arreterAlarme();
      } else {
        // Bouton RELÂCHÉ -> publie command = "off"
        Serial.println("[BOUTON] Relache -> command = off");
        if (client.connected()) {
          client.publish(topic_btn_command, "off");
        }
      }
    }
  }

  // -------------------------------------
  // 2. SIRÈNE ET CLIGNOTEMENT SI ALARME ACTIVE
  // -------------------------------------
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