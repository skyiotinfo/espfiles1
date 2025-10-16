/**
 * The example to stream changes to a single location in Realtime Database.
 *
 * This example uses the UserAuth class for authentication.
 * See examples/App/AppInitialization for more authentication examples.
 *
 * For the complete usage guidelines, please read README.md or visit https://github.com/mobizt/FirebaseClient
 */
 
// https://esp-02-userdata.asia-southeast1.firebasedatabase.app/board1/outputs/digital1/tppzVAZgZ2exD726eIKQgzYeJrl2.json?auth={{AUTH_TOKEN}}
 
#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
 
#include <FirebaseClient.h>
#include "ExampleFunctions.h" // Provides the functions used in the examples.
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
 
// Network and Firebase credentials
#define WIFI_SSID "Galaxy M42"
#define WIFI_PASSWORD "Chai1111"
const int outpin = D8;
 
#define API_KEY "AIzaSyBSypmrtk19MJBkQ-UcMsuY8KFYQLlk8xw"
#define DATABASE_URL "https://esp-02-userdata.asia-southeast1.firebasedatabase.app"
#define USER_EMAIL "u9764005401@gmail.com"
#define USER_PASSWORD "admin123"
 
String uid;
String path;
int wifi_status = 0;
int wifi_timeout = 40;
int count=0;
 
void processData(AsyncResult &aResult);
 
SSL_CLIENT ssl_client, stream_ssl_client;
 
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client), streamClient(stream_ssl_client);
 
UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD, 3000 /* expire period in seconds (<3600) */);
FirebaseApp app;
RealtimeDatabase Database;
AsyncResult streamResult;
 
unsigned long ms = 0;
void check_wifi_connection() {
    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.print(".");
        Serial.println(wifi_timeout);
        delay(500);
        wifi_timeout--;
        if (wifi_timeout <= 0){
            wifi_timeout=40;
            break;
        }
    }
}
 
void setup()
{
    Serial.begin(115200);
    pinMode(outpin, OUTPUT);
    digitalWrite(outpin, HIGH);
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD); 
    Serial.println("Connecting to Wi-Fi");
    check_wifi_connection();
    Serial.println("Connected with IP: ");
    Serial.println(WiFi.localIP());
    Serial.println();
    Firebase.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);
    set_ssl_client_insecure_and_buffer(ssl_client);
    set_ssl_client_insecure_and_buffer(stream_ssl_client);
    Serial.println("Initializing app...");
    //initializeApp(aClient, app, getAuth(user_auth), auth_debug_print, "🔐 authTask");
    // Or intialize the app and wait.
    initializeApp(aClient, app, getAuth(user_auth), 20 * 1000, auth_debug_print);
    app.getApp<RealtimeDatabase>(Database);
    Database.url(DATABASE_URL);
    // In SSE mode (HTTP Streaming) task, you can filter the Stream events by using AsyncClientClass::setSSEFilters(<keywords>),
    // which the <keywords> is the comma separated events.
    // The event keywords supported are:
    // get - To allow the http get response (first put event since stream connected).
    // put - To allow the put event.
    // patch - To allow the patch event.
    // keep-alive - To allow the keep-alive event.
    // cancel - To allow the cancel event.
    // auth_revoked - To allow the auth_revoked event.
    // To clear all prevousely set filter to allow all Stream events, use AsyncClientClass::setSSEFilters().
    streamClient.setSSEFilters("get,put,patch,keep-alive,cancel,auth_revoked");
 
    // The "unauthenticate" error can be occurred in this case because we don't wait
    // the app to be authenticated before connecting the stream.
    // This is ok as stream task will be reconnected automatically when the app is authenticated.
    // The streamClient must be used for Stream only.
    // Database.get(streamClient, path, processData, true /* SSE mode (HTTP Streaming) */, "streamTask");
 
    // Async call with AsyncResult for returning result.
    // Database.get(streamClient, "/examples/Stream/data", streamResult, true /* SSE mode (HTTP Streaming) */);
    // path = "/board1/outputs/digital/tppzVAZgZ2exD726eIKQgzYeJrl2";
 
    uid = app.getUid().c_str();
    path = "/board1/outputs/digital/" + uid;
    Serial.println(path);
    Database.get(streamClient, path, processData, true /* SSE mode (HTTP Streaming) */, "streamTask");
}
 
void loop()
{
    // To maintain the authentication and async tasks
    app.loop();
    if (WiFi.status() != WL_CONNECTED){
        check_wifi_connection();
    }        

    if (app.ready() && millis() - ms > 20000)
    {

        ms = millis();
        //JsonWriter writer;
        //object_t json, obj1, obj2;
        //writer.create(obj1, "12", 1);
        //writer.create(obj2, "13", 0);
        //writer.join(json, 2, obj1, obj2);
        //uid = app.getUid().c_str();
        path = "/board1/outputs/digital/" + uid;
        Serial.println(path);
        //Database.set<object_t>(aClient, path, json, processData, "setTask");
    }
    // For async call with AsyncResult.
    // processData(streamResult);
}
 
void processData(AsyncResult &aResult)
{
    // Exits when no result is available when calling from the loop.
    if (!aResult.isResult())
        return;
    if (aResult.isEvent())
    {
        Firebase.printf("Event task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.eventLog().message().c_str(), aResult.eventLog().code());
    }
    if (aResult.isDebug())
    {
        Firebase.printf("Debug task: %s, msg: %s\n", aResult.uid().c_str(), aResult.debug().c_str());
    }
    if (aResult.isError())
    {
        Firebase.printf("Error task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
    }
    if (aResult.available())
    {
        RealtimeDatabaseResult &stream = aResult.to<RealtimeDatabaseResult>();
        if (stream.isStream())
        {
            Serial.println("----------------------------");
            Firebase.printf("task: %s\n", aResult.uid().c_str());
            Firebase.printf("event: %s\n", stream.event().c_str());
            Firebase.printf("path: %s\n", stream.dataPath().c_str());
            Firebase.printf("data: %s\n", stream.to<const char *>());
            Firebase.printf("type: %d\n", stream.type());
            int temp = stream.to<int>();
            Serial.println(temp);   
            if(temp==11){
                digitalWrite(outpin, HIGH);
            }
            else if(temp==22){
                digitalWrite(outpin, LOW);
            }
            // The stream event from RealtimeDatabaseResult can be converted to the values as following.
            bool v1 = stream.to<bool>();
            int v2 = stream.to<int>();
            float v3 = stream.to<float>();
            double v4 = stream.to<double>();
            String v5 = stream.to<String>();
        }
        else
        {
            Serial.println("----------------------------");
            Firebase.printf("task: %s, payload: %s\n", aResult.uid().c_str(), aResult.c_str());
        }
#if defined(ESP32) || defined(ESP8266)
        Firebase.printf("Free Heap: %d\n", ESP.getFreeHeap());
#elif defined(ARDUINO_RASPBERRY_PI_PICO_W)
        Firebase.printf("Free Heap: %d\n", rp2040.getFreeHeap());
#endif
    }
}