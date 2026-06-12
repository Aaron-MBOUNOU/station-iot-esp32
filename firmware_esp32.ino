// =============================================================================
// firmware_esp32.ino — Firmware principal ESP32 DWP
// Projet BTS CIEL IR — Aaron MBOUNOU — NaTran
//
// Fonctionnement :
//   1. Connexion WiFi sécurisée (WPA2)
//   2. Connexion MQTT over TLS (MQTTS port 8883)
//   3. Lecture DS18B20 (température précise) + DHT22 (temp + humidité)
//   4. Publication des mesures toutes les 30 secondes
//   5. Écoute du topic "commande" pour piloter le relais
//   6. Détection des seuils → publication d'alertes automatiques
//
// Librairies Arduino IDE à installer (Library Manager) :
//   - PubSubClient       (Nick O'Leary)
//   - OneWire            (Jim Studt)
//   - DallasTemperature  (Miles Burton)
//   - DHT sensor library (Adafruit)
//   - ArduinoJson        (Benoit Blanchon)
//
// Carte ciblée : ESP32 Dev Module (via esp32 by Espressif, Board Manager)
// =============================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include "config.h"

// =============================================================================
// OBJETS GLOBAUX
// =============================================================================

// --- Capteurs ---
OneWire          oneWire(PIN_DS18B20);
DallasTemperature capteurDS18B20(&oneWire);
DHT              capteurDHT(PIN_DHT22, DHT22);

// --- Réseau ---
WiFiClientSecure clientTLS;
PubSubClient     clientMQTT(clientTLS);

// --- Timers ---
unsigned long derniereMesure  = 0;
unsigned long dernierStatut   = 0;

// --- État du relais ---
bool relaisActif = false;

// =============================================================================
// FONCTIONS WIFI
// =============================================================================

/**
 * Connexion au réseau WiFi avec timeout.
 * Clignote la LED de statut pendant la tentative.
 */
void connecterWifi() {
    Serial.println("\n[WiFi] Connexion à " + String(WIFI_SSID) + "...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long debut = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - debut > TIMEOUT_WIFI) {
            Serial.println("[WiFi] ÉCHEC — Timeout dépassé. Redémarrage...");
            delay(1000);
            ESP.restart();
        }
        digitalWrite(PIN_LED_STAT, !digitalRead(PIN_LED_STAT)); // Clignotement
        delay(500);
        Serial.print(".");
    }

    digitalWrite(PIN_LED_STAT, HIGH); // LED fixe = connecté
    Serial.println("\n[WiFi] Connecté !");
    Serial.println("  IP locale  : " + WiFi.localIP().toString());
    Serial.println("  MAC        : " + WiFi.macAddress());
    Serial.println("  RSSI       : " + String(WiFi.RSSI()) + " dBm");
}

// =============================================================================
// FONCTIONS MQTT
// =============================================================================

/**
 * Callback appelé à chaque message reçu sur un topic souscrit.
 * Gère les ordres de pilotage du relais et autres commandes.
 */
void callbackMQTT(char* topic, byte* payload, unsigned int longueur) {
    String topicStr  = String(topic);
    String messageStr = "";
    for (unsigned int i = 0; i < longueur; i++) {
        messageStr += (char)payload[i];
    }

    Serial.println("[MQTT] Message reçu sur : " + topicStr);
    Serial.println("       Contenu : " + messageStr);

    // --- Traitement des commandes relais ---
    if (topicStr == String(TOPIC_COMMANDE)) {
        // Parser le JSON de commande
        // Format attendu : {"action": "ON"} ou {"action": "OFF"}
        StaticJsonDocument<128> doc;
        DeserializationError err = deserializeJson(doc, messageStr);

        if (err) {
            Serial.println("[MQTT] Erreur parsing JSON commande : " + String(err.c_str()));
            return;
        }

        String action = doc["action"] | "INCONNU";

        if (action == "ON") {
            digitalWrite(PIN_RELAIS, HIGH);
            relaisActif = true;
            Serial.println("[RELAIS] Activé par commande MQTT");
            publierStatut(); // Confirmer immédiatement l'état
        }
        else if (action == "OFF") {
            digitalWrite(PIN_RELAIS, LOW);
            relaisActif = false;
            Serial.println("[RELAIS] Désactivé par commande MQTT");
            publierStatut();
        }
        else if (action == "REBOOT") {
            Serial.println("[CMD] Redémarrage demandé via MQTT...");
            delay(500);
            ESP.restart();
        }
        else {
            Serial.println("[MQTT] Commande inconnue : " + action);
        }
    }
}

/**
 * Connexion au broker MQTT avec TLS et authentification.
 * Se reconnecte automatiquement en cas de perte de connexion.
 */
void connecterMQTT() {
    // Configurer le certificat CA pour valider le broker
    clientTLS.setCACert(MQTT_CA_CERT);
    // Si vous avez aussi un certificat client (mTLS) :
    // clientTLS.setCertificate(CLIENT_CERT);
    // clientTLS.setPrivateKey(CLIENT_KEY);

    clientMQTT.setServer(MQTT_BROKER_IP, MQTT_BROKER_PORT);
    clientMQTT.setCallback(callbackMQTT);
    clientMQTT.setKeepAlive(60);
    clientMQTT.setBufferSize(512);

    int tentatives = 0;
    while (!clientMQTT.connected()) {
        tentatives++;
        Serial.print("[MQTT] Tentative de connexion #" + String(tentatives) + "...");

        // Message de dernière volonté (LWT) — publié si l'ESP32 se déconnecte brusquement
        String lwtPayload = "{\"statut\":\"OFFLINE\",\"device\":\"" + String(DEVICE_ID) + "\"}";

        bool connecte = clientMQTT.connect(
            MQTT_CLIENT_ID,
            MQTT_USERNAME,
            MQTT_PASSWORD,
            TOPIC_STATUT,           // Topic LWT
            1,                      // QoS LWT
            true,                   // Retain LWT
            lwtPayload.c_str()
        );

        if (connecte) {
            Serial.println(" Connecté !");

            // S'abonner au topic de commandes
            clientMQTT.subscribe(TOPIC_COMMANDE, 1);
            Serial.println("[MQTT] Souscription à : " + String(TOPIC_COMMANDE));

            // Publier statut ONLINE
            publierStatut();
        }
        else {
            Serial.println(" Échec (code=" + String(clientMQTT.state()) + ")");
            Serial.println("[MQTT] Retry dans " + String(DELAI_RECONNEXION/1000) + "s...");
            delay(DELAI_RECONNEXION);

            if (tentatives >= 5) {
                Serial.println("[MQTT] 5 échecs — Redémarrage ESP32...");
                delay(1000);
                ESP.restart();
            }
        }
    }
}

// =============================================================================
// FONCTIONS PUBLICATION MQTT
// =============================================================================

/**
 * Publie les mesures de température et d'humidité au format JSON.
 * Format JSON publié :
 * {
 *   "device"      : "ESP32-ACACIA-001",
 *   "location"    : "Salle Acacia 0.02",
 *   "timestamp_ms": 123456,
 *   "temperature" : 22.5,
 *   "humidite"    : 45.2,
 *   "relais"      : false
 * }
 */
void publierMesures(float temp_ds18b20, float temp_dht, float humidite) {
    StaticJsonDocument<256> doc;

    doc["device"]       = DEVICE_ID;
    doc["location"]     = DEVICE_LOCATION;
    doc["floor"]        = DEVICE_FLOOR;
    doc["timestamp_ms"] = millis();
    doc["temp_ds18b20"] = serialized(String(temp_ds18b20, 2));  // Capteur précis
    doc["temp_dht22"]   = serialized(String(temp_dht, 1));      // Capteur ambiant
    doc["humidite"]     = serialized(String(humidite, 1));
    doc["relais"]       = relaisActif;
    doc["rssi_dbm"]     = WiFi.RSSI();

    char buffer[256];
    serializeJson(doc, buffer);

    // Publier température (DS18B20 = plus précis)
    clientMQTT.publish(TOPIC_TEMPERATURE, buffer, false); // QoS 0, pas de retain
    Serial.println("[MQTT] → " + String(TOPIC_TEMPERATURE) + " : " + buffer);

    // Vérification des seuils d'alerte
    verifierSeuils(temp_ds18b20, humidite);
}

/**
 * Publie le statut général de l'équipement.
 */
void publierStatut() {
    StaticJsonDocument<200> doc;

    doc["device"]    = DEVICE_ID;
    doc["location"]  = DEVICE_LOCATION;
    doc["statut"]    = "ONLINE";
    doc["relais"]    = relaisActif;
    doc["rssi_dbm"]  = WiFi.RSSI();
    doc["uptime_ms"] = millis();
    doc["ip"]        = WiFi.localIP().toString();
    doc["mac"]       = WiFi.macAddress();

    char buffer[200];
    serializeJson(doc, buffer);

    clientMQTT.publish(TOPIC_STATUT, buffer, true); // Retain = true (dernier état connu)
    Serial.println("[MQTT] → Statut publié");
}

/**
 * Vérifie les seuils et publie une alerte si dépassé.
 */
void verifierSeuils(float temperature, float humidite) {
    String typeAlerte = "";
    String niveauAlerte = "";

    if (temperature >= SEUIL_TEMP_MAX) {
        typeAlerte   = "SURCHAUFFE";
        niveauAlerte = "CRITIQUE";
    }
    else if (temperature <= SEUIL_TEMP_MIN) {
        typeAlerte   = "TEMPERATURE_BASSE";
        niveauAlerte = "AVERTISSEMENT";
    }
    else if (humidite >= SEUIL_HUMID_MAX) {
        typeAlerte   = "HUMIDITE_HAUTE";
        niveauAlerte = "AVERTISSEMENT";
    }
    else if (humidite <= SEUIL_HUMID_MIN) {
        typeAlerte   = "AIR_SEC";
        niveauAlerte = "AVERTISSEMENT";
    }

    if (typeAlerte != "") {
        StaticJsonDocument<200> alerte;
        alerte["device"]      = DEVICE_ID;
        alerte["location"]    = DEVICE_LOCATION;
        alerte["alerte"]      = typeAlerte;
        alerte["niveau"]      = niveauAlerte;
        alerte["temperature"] = serialized(String(temperature, 2));
        alerte["humidite"]    = serialized(String(humidite, 1));

        char buffer[200];
        serializeJson(alerte, buffer);

        clientMQTT.publish(TOPIC_ALERTE, buffer, false);
        Serial.println("[ALERTE] 🚨 " + typeAlerte + " détectée ! → " + String(TOPIC_ALERTE));
    }
}

// =============================================================================
// LECTURE CAPTEURS
// =============================================================================

/**
 * Lit le capteur DS18B20 (température précise via OneWire).
 * Retourne -999.0 en cas d'erreur.
 */
float lireDS18B20() {
    capteurDS18B20.requestTemperatures();
    float temp = capteurDS18B20.getTempCByIndex(0);

    if (temp == DEVICE_DISCONNECTED_C) {
        Serial.println("[DS18B20] ERREUR — Capteur déconnecté !");
        return -999.0;
    }
    return temp;
}

/**
 * Lit le capteur DHT22 (température + humidité).
 * Retourne {-999.0, -999.0} en cas d'erreur.
 */
struct MesureDHT { float temperature; float humidite; };

MesureDHT lireDHT22() {
    MesureDHT mesure;
    mesure.humidite    = capteurDHT.readHumidity();
    mesure.temperature = capteurDHT.readTemperature();

    if (isnan(mesure.humidite) || isnan(mesure.temperature)) {
        Serial.println("[DHT22] ERREUR — Lecture impossible !");
        mesure.temperature = -999.0;
        mesure.humidite    = -999.0;
    }
    return mesure;
}

// =============================================================================
// SETUP & LOOP
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║   Firmware ESP32 DWP — NaTran        ║");
    Serial.println("║   Aaron MBOUNOU — BTS CIEL IR        ║");
    Serial.println("╚══════════════════════════════════════╝");
    Serial.println("Device : " + String(DEVICE_ID));
    Serial.println("Salle  : " + String(DEVICE_LOCATION));

    // --- Initialisation des broches ---
    pinMode(PIN_RELAIS,   OUTPUT);
    pinMode(PIN_LED_STAT, OUTPUT);
    digitalWrite(PIN_RELAIS,   LOW);  // Relais OFF au démarrage
    digitalWrite(PIN_LED_STAT, LOW);

    // --- Initialisation des capteurs ---
    capteurDS18B20.begin();
    capteurDHT.begin();
    Serial.println("[Capteurs] DS18B20 et DHT22 initialisés");

    // --- Connexion WiFi ---
    connecterWifi();

    // --- Connexion MQTT over TLS ---
    connecterMQTT();

    Serial.println("[Setup] Initialisation terminée ✓");
    Serial.println("─────────────────────────────────────────");
}

void loop() {
    // Maintien de la connexion MQTT
    if (!clientMQTT.connected()) {
        Serial.println("[Loop] Connexion MQTT perdue — Reconnexion...");
        connecterMQTT();
    }
    clientMQTT.loop();

    unsigned long maintenant = millis();

    // --- Publication des mesures toutes les INTERVALLE_MESURE ms ---
    if (maintenant - derniereMesure >= INTERVALLE_MESURE) {
        derniereMesure = maintenant;

        float tempDS = lireDS18B20();
        MesureDHT dht = lireDHT22();

        Serial.println("\n── Mesure ──────────────────────────────");
        Serial.println("  DS18B20 : " + String(tempDS, 2) + " °C");
        Serial.println("  DHT22   : " + String(dht.temperature, 1) + " °C  |  " + String(dht.humidite, 1) + " %");
        Serial.println("  Relais  : " + String(relaisActif ? "ACTIF" : "INACTIF"));

        if (tempDS != -999.0 && dht.humidite != -999.0) {
            publierMesures(tempDS, dht.temperature, dht.humidite);
        }
    }

    // --- Publication du statut toutes les INTERVALLE_STATUT ms ---
    if (maintenant - dernierStatut >= INTERVALLE_STATUT) {
        dernierStatut = maintenant;
        publierStatut();
    }
}
