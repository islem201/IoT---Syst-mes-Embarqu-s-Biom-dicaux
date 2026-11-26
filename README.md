#  Système Embarqué Biomédical IoT — Monitoring Vital, Détection de Chute & Diagnostic I2C

![Statut](https://img.shields.io/badge/Statut-Opérationnel-brightgreen)
![Plateforme](https://img.shields.io/badge/Hardware-3x%20ESP32%20%2B%20Raspberry%20Pi%204-blue)
![Protocole](https://img.shields.io/badge/Protocole-I2C%20%7C%20MQTT%20%7C%20JSON-orange)
![Supervision](https://img.shields.io/badge/Dashboard-Node--RED%202.0-red)

Mini-projet réalisé dans le cadre de la formation d'ingénieur en **Informatique et Systèmes d'Information pour la Santé (ISIS — Castres, 2026)**.

Ce système IoT distribué assure la surveillance physiologique en temps réel d'un patient (fréquence cardiaque, saturation en oxygène $SpO_2$), la détection de chute ou de choc brusque, le diagnostic matériel continu via bus I2C, ainsi que le déclenchement d'une alarme sonore et visuelle acquittable physiquement.

---

##  Documentation Complète (Wiki GitHub)

L'ensemble de la documentation détaillée (tutoriel pas-à-pas, schémas électriques Cirkit Designer, explications du code, captures Node-RED / MQTT Explorer et validation sur moniteurs série) est disponible dans l'onglet **[Wiki du dépôt](../../wiki)**.

---

##  Architecture du Système

Le projet repose sur **trois nœuds ESP32** spécialisés et un **serveur central Raspberry Pi 4** (`172.18.32.42`) :

* **ESP32 #1 — Nœud Principal (Esclave I2C `0x55` & Émetteur MQTT) :**
  * Acquiert toutes les `1 s` les données du capteur cardiaque/oxymètre **DFRobot MAX30102** (`0x57`) et de l'accéléromètre/gyroscope **Joy-it MPU6050** (`0x68`), chaînés sur un bus I2C secondaire (`GPIO 25` / `GPIO 26` en mode *I2C Bypass*).
  * Vérifie à chaud la présence physique des capteurs et met à jour dynamiquement un code d'erreur (`0` à `10`).
  * Publie les mesures (valeurs brutes et objets **JSON**) vers le broker Mosquitto.
* **ESP32 #2 — Nœud de Diagnostic (Maître I2C — Hors Wi-Fi) :**
  * Interroge l'ESP32 #1 toutes les `2 s` via le bus I2C principal filaire (`GPIO 21` / `GPIO 22` + `GND` commun).
  * Fonctionne indépendamment du réseau Wi-Fi pour diagnostiquer les pannes matérielles (déconnexion d'un capteur `Code 2`, coupure du câble I2C `Code 8`, perte Wi-Fi `Code 1`, etc.) sur console série USB (`115200 bauds`).
* **ESP32 #3 — Nœud de Pilotage & Alarme (Abonné MQTT) :**
  * Écoute les ordres d'alerte émis par Node-RED sur `esp32/alarme/commande`.
  * Déclenche de manière non-bloquante (`millis()`) une LED rouge clignotante (`GPIO 25`) et une sirène modulée sur un buzzer passif **GT520** (`GPIO 27`, alternance `1400 Hz` / `2200 Hz`).
  * Permet l'acquittement local via un bouton poussoir (`GPIO 26`, `INPUT_PULLUP`) publiant `"on"` / `"off"` sur `esp32/alarme/command`.
* **Raspberry Pi 4 — Broker Mosquitto & Node-RED Dashboard 2.0 :**
  * Centralise les flux MQTT (`port 1883`), calcule la norme d'accélération $\sqrt{x^2+y^2+z^2}$, vérifie les seuils cliniques ($BPM < 45$ ou $> 120$, $SpO_2 < 92\%$, choc $> 18\,m/s^2$), affiche les courbes/jauges en direct et archive l'historique au format CSV (`mesures_sante.csv`).



https://github.com/user-attachments/assets/3085b614-1cae-4667-9994-d0454c2540bb


---

## Structure du Dépôt

```text
├── code_esp32/
│   ├── esp32_slave_capteurs.ino       # Code ESP32 #1 : MAX30102 + MPU6050 + Esclave I2C 0x55 + MQTT
│   ├── esp32_master_diagnostic.ino    # Code ESP32 #2 : Maître I2C + Console de diagnostic matériel
│   └── esp32_pilotage_alarme.ino      # Code ESP32 #3 : LED + Buzzer GT520 + Bouton d'acquittement
├── node_red/
│   └── flow_sante_v5.json             # Flux Node-RED complet (Dashboard 2.0, Seuils, Journal & CSV)
├── images/                            # Schémas Cirkit Designer, architecture TikZ, captures & photos
└── README.md                          # Présentation générale du projet
