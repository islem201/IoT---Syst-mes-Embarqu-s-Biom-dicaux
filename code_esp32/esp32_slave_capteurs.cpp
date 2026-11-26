#include <WiFi.h>
#include <PubSubClient.h>
#include "Wire.h"
#include "DFRobot_BloodOxygen_S.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// =====================================
// CONFIGURATION WIFI & MQTT (Paramètres TP)
// =====================================
const char* ssid        = "iot";
const char* password    = "iotisis;";
const char* mqtt_server = "172.18.32.42";
const int   mqtt_port   = 1883;

// Topics MQTT publiés vers le Raspberry Pi
const char* topic_spo2   = "esp32/sante/spo2";
const char* topic_bpm    = "esp32/sante/bpm";
const char* topic_temp   = "esp32/sante/temperature";
const char* topic_accel  = "esp32/mpu6050/accel";
const char* topic_gyro   = "esp32/mpu6050/gyro";
const char* topic_status = "esp32/diagnostic/erreur";

WiFiClient espClient;
PubSubClient client(espClient);

// =====================================
// CONFIGURATION I2C (BUS 0 = SLAVE / BUS 1 = CAPTEURS)
// =====================================
#define I2C_DEV_ADDR  0x55  // Adresse de cet ESP32 Slave sur Bus 0 (IO21/IO22)
#define SLAVE_SDA     21
#define SLAVE_SCL     22

#define MAX30102_ADDR 0x57  // Adresse MAX30102 sur Bus 1 (IO25/IO26)
#define MPU6050_ADDR  0x68  // Adresse MPU6050 sur Bus 1 (IO25/IO26)
#define SENSORS_SDA   25
#define SENSORS_SCL   26

TwoWire I2C_2 = TwoWire(1);
DFRobot_BloodOxygen_S_I2C MAX30102(&I2C_2, MAX30102_ADDR);
Adafruit_MPU6050 mpu;

// =====================================
// TABLE DES CODES ERREUR
// =====================================
#define ERR_NONE              0
#define ERR_WIFI_LOST         1
#define ERR_SENSOR_DISCONNECT 2
#define ERR_SENSOR_READ       3
#define ERR_TEMP_LIMIT        4
#define ERR_LOW_VOLTAGE       5
#define ERR_SERIAL_LOST       6
#define ERR_LOW_MEMORY        7
#define ERR_I2C               8
#define ERR_WATCHDOG          9
#define ERR_UNKNOWN           10

volatile uint8_t errorCode = ERR_NONE;

bool maxConnected = false;
bool mpuConnected = false;

// Mémoire tampon pour éviter les -1 transitoires entre BPM et SpO2
int dernierBpmValide = -1;
int dernierSpo2Valide = -1;
uint8_t compteurSansDoigt = 0;

unsigned long lastSensorCycle = 0;
unsigned long lastMqttReconnectAttempt = 0;
const unsigned long INTERVALLE_ENVOI = 1000;

// =====================================
// FONCTIONS I2C SLAVE (DIALOGUE AVEC MASTER)
// =====================================
void updateSlaveTxBuffer(uint8_t code) {
  errorCode = code;
  uint8_t buf[1] = { code };
  Wire.slaveWrite(buf, 1);
}

void onReceive(int len) {
  while (Wire.available()) {
    Wire.read();
  }
}

void onRequest() {
  uint8_t code = errorCode;
  Wire.write(&code, 1);
}

// =====================================
// INITIALISATION / RÉVEIL COMPLET DU MPU6050
// =====================================
bool initMPU6050() {
  if (mpu.begin(MPU6050_ADDR, &I2C_2)) {
    mpu.enableSleep(false); // Sort explicitement du mode veille après coupure VCC
    mpu.setI2CBypass(true); // Maintient l'accès au MAX30102 chaîné en série
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    return true;
  }
  return false;
}

// =====================================
// VÉRIFICATION MATÉRIELLE SUR LE BUS I2C_2
// =====================================
bool checkI2CDevice(uint8_t address) {
  I2C_2.beginTransmission(address);
  return (I2C_2.endTransmission() == 0);
}

// =====================================
// GESTION WIFI & MQTT (NON-BLOQUANTE)
// =====================================
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 8000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nEchec connexion WiFi (passage en mode degrade)");
    errorCode = ERR_WIFI_LOST;
  }
}

void callback(char* topic, byte* message, unsigned int length) {
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)message[i]);
  }
  Serial.println();
}

void reconnect() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnectAttempt >= 5000) {
      lastMqttReconnectAttempt = now;
      Serial.print("Attempting MQTT connection...");
      if (client.connect("ESP32Client_SanteSlave")) {
        Serial.println("connected");
        client.subscribe("esp32/output");
      } else {
        Serial.print("failed, rc=");
        Serial.print(client.state());
        Serial.println(" try again in 5 seconds");
      }
    }
  }
}

// =====================================
// INITIALISATION
// =====================================
void setup() {
  Serial.begin(115200);

  // 1. Démarrage du Bus I2C 1 (Capteurs sur IO25/IO26)
  I2C_2.begin(SENSORS_SDA, SENSORS_SCL, 100000);

  if (initMPU6050()) {
    mpuConnected = true;
    Serial.println("Joy-it MPU6050 initialise avec succes !");
  } else {
    mpuConnected = false;
    errorCode = ERR_SENSOR_DISCONNECT;
    Serial.println("ERREUR : Joy-it MPU6050 non detecte !");
  }

  if (MAX30102.begin()) {
    maxConnected = true;
    MAX30102.sensorStartCollect();
    Serial.println("MAX30102 initialise avec succes !");
  } else {
    maxConnected = false;
    errorCode = ERR_SENSOR_DISCONNECT;
    Serial.println("ERREUR : MAX30102 non detecte !");
  }

  // 2. Connexion WiFi & Configuration MQTT
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  // 3. Démarrage du Bus I2C 0 en mode SLAVE (IO21/IO22)
  Wire.onReceive(onReceive);
  Wire.onRequest(onRequest);
  Wire.begin((uint8_t)I2C_DEV_ADDR, SLAVE_SDA, SLAVE_SCL, 100000);
  updateSlaveTxBuffer(errorCode);

  Serial.println("ESP32 Slave initialise et pret pour le Master (0x55)");
}

// =====================================
// BOUCLE PRINCIPALE
// =====================================
void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      reconnect();
    }
    client.loop();
  }

  if (millis() - lastSensorCycle >= INTERVALLE_ENVOI) {
    lastSensorCycle = millis();

    uint8_t currentError = ERR_NONE;

    // -------------------------------------------------
    // 1. VÉRIFICATION DÉCONNEXION MATÉRIELLE (ERREUR 2)
    // -------------------------------------------------
    bool mpuPresent = checkI2CDevice(MPU6050_ADDR);
    bool maxPresent = checkI2CDevice(MAX30102_ADDR);

    if (!maxPresent || !mpuPresent) {
      currentError = ERR_SENSOR_DISCONNECT; // Code 2
      if (!maxPresent) {
        Serial.println("[DIAGNOSTIC] Alerte : MAX30102 deconnecte !");
        maxConnected = false;
        dernierBpmValide = -1;
        dernierSpo2Valide = -1;
        compteurSansDoigt = 0;
      }
      if (!mpuPresent) {
        Serial.println("[DIAGNOSTIC] Alerte : MPU6050 deconnecte !");
        mpuConnected = false;
      }
    }

    // Reconnexion automatique après débranchement SDA/SCL (Erreur 2 -> 0)
    if (mpuPresent && !mpuConnected) {
      if (initMPU6050()) {
        mpuConnected = true;
        Serial.println("[DIAGNOSTIC] MPU6050 reconnecte et configure !");
      }
    }
    if (maxPresent && !maxConnected) {
      if (MAX30102.begin()) {
        MAX30102.sensorStartCollect();
        maxConnected = true;
        Serial.println("[DIAGNOSTIC] MAX30102 reconnecte !");
      }
    }

    // -------------------------------------------------
    // 2. LECTURE DES CAPTEURS & AUTO-RÉVEIL (ERREUR 3)
    // -------------------------------------------------
    int spo2 = -1;
    int bpm = -1;
    float boardTemp = 0.0;
    sensors_event_t a, g, mpuTemp;

    if (maxConnected) {
      MAX30102.getHeartbeatSPO2();
      int rawSpo2 = MAX30102._sHeartbeatSPO2.SPO2;
      int rawBpm  = MAX30102._sHeartbeatSPO2.Heartbeat;
      boardTemp   = MAX30102.getTemperature_C();

      // Maintien de la dernière valeur valide si l'un des deux passe temporairement à -1
      if (rawBpm > 0 || rawSpo2 > 0) {
        compteurSansDoigt = 0;
        if (rawBpm > 0)  dernierBpmValide  = rawBpm;
        if (rawSpo2 > 0) dernierSpo2Valide = rawSpo2;
      } else {
        // Les deux sont à -1 : on attend 5 secondes (5 cycles) avant de considérer le doigt retiré
        compteurSansDoigt++;
        if (compteurSansDoigt >= 5) {
          dernierBpmValide  = -1;
          dernierSpo2Valide = -1;
        }
      }

      bpm  = dernierBpmValide;
      spo2 = dernierSpo2Valide;

      Serial.printf("MAX30102 -> SpO2: %d %% (brut:%d) | BPM: %d (brut:%d) | Temp: %.1f C\n",
                    spo2, rawSpo2, bpm, rawBpm, boardTemp);

      if (boardTemp > 55.0 && currentError == ERR_NONE) {
        currentError = ERR_TEMP_LIMIT; // Code 4
      }
    }

    if (mpuConnected) {
      bool lectureOk = mpu.getEvent(&a, &g, &mpuTemp);
      bool donneesNulles = (a.acceleration.x == 0.0 && a.acceleration.y == 0.0 && a.acceleration.z == 0.0);

      // Si données nulles (ex: MPU6050 repassé en veille après coupure VCC), on tente un réveil immédiat
      if (!lectureOk || donneesNulles) {
        Serial.println("[DIAGNOSTIC] Lecture nulle/echouee sur MPU6050 -> tentative de reveil...");
        if (initMPU6050()) {
          delay(10);
          lectureOk = mpu.getEvent(&a, &g, &mpuTemp);
          donneesNulles = (a.acceleration.x == 0.0 && a.acceleration.y == 0.0 && a.acceleration.z == 0.0);
        }
      }

      if (lectureOk && !donneesNulles) {
        Serial.printf("MPU6050  -> Accel X:%.2f Y:%.2f Z:%.2f | Gyro X:%.2f Y:%.2f Z:%.2f\n",
                      a.acceleration.x, a.acceleration.y, a.acceleration.z,
                      g.gyro.x, g.gyro.y, g.gyro.z);
      } else if (currentError == ERR_NONE) {
        currentError = ERR_SENSOR_READ; // Code 3 tant que le capteur ne renvoie pas de valeurs valides
        Serial.println("[DIAGNOSTIC] Alerte : Erreur de lecture MPU6050 (Code 3) !");
      }
    }

    // -------------------------------------------------
    // 3. VÉRIFICATION RÉSEAU & MÉMOIRE
    // -------------------------------------------------
    if (WiFi.status() != WL_CONNECTED && currentError == ERR_NONE) {
      currentError = ERR_WIFI_LOST; // Code 1
    }

    if (ESP.getFreeHeap() < 20000 && currentError == ERR_NONE) {
      currentError = ERR_LOW_MEMORY; // Code 7
    }

    // Mise à jour atomique + pré-chargement du tampon TX I2C pour le Master
    updateSlaveTxBuffer(currentError);
    Serial.printf(">>> Code erreur transmis au Master : %d\n", errorCode);

    // -------------------------------------------------
    // 4. PUBLICATION DES TOPICS VERS LE RASPBERRY PI
    // -------------------------------------------------
    if (client.connected()) {
      char msgBuffer[80];

      if (maxConnected) {
        snprintf(msgBuffer, sizeof(msgBuffer), "%d", bpm);
        client.publish(topic_bpm, msgBuffer);

        snprintf(msgBuffer, sizeof(msgBuffer), "%d", spo2);
        client.publish(topic_spo2, msgBuffer);

        snprintf(msgBuffer, sizeof(msgBuffer), "%.1f", boardTemp);
        client.publish(topic_temp, msgBuffer);
      }

      if (mpuConnected) {
        snprintf(msgBuffer, sizeof(msgBuffer), "{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}",
                 a.acceleration.x, a.acceleration.y, a.acceleration.z);
        client.publish(topic_accel, msgBuffer);

        snprintf(msgBuffer, sizeof(msgBuffer), "{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}",
                 g.gyro.x, g.gyro.y, g.gyro.z);
        client.publish(topic_gyro, msgBuffer);
      }

      snprintf(msgBuffer, sizeof(msgBuffer), "%d", errorCode);
      client.publish(topic_status, msgBuffer);

      Serial.println("[MQTT] Donnees publiees (1s)");
    }
  }
}
