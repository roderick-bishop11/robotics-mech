/*
In this program, we will test the ESP32 by creating a simple web server that can be accessed from any device connected to the same router. 
The web server will display LED ON and LED OFF buttons. 
Clicking these buttons will control the onboard LED of the ESP32, which is connected to GPIO2.
When you click LED ON, the onboard LED will turn on, and clicking LED OFF will turn it off. 
This provides a simple and interactive way to test the ESP32's functionality.
*/
#include <WiFi.h> //c like structure, very interesting
#include <WebServer.h>
#include "secrets.h" //include secrets.h, ssid and password are available here. 

WebServer server(80); // Web server on port 80

const int ledPin = 2; // Onboard LED pin (GPIO2 for ESP32) <specify the pins here, you can use a simple number
bool ledState = false; // Track LED state
unsigned long startTime = millis();

// HTML content for the web page
String HTMLPage() {
  String html = "<!DOCTYPE html><html>";
  html += "<head><title>ESP32 LED Control</title></head>";
  html += "<body style='text-align: center; font-family: Arial;'>";

  html += "<h1>ESP32 LED Control</h1>";
  html += ledState ? "<p>LED is <strong>ON</strong></p>" : "<p>LED is <strong>OFF</strong></p>";
  html += "<a href='/on' style='padding: 10px 20px; background-color: green; color: white; text-decoration: none;'>Turn ON</a>&nbsp;";
  html += "<a href='/off' style='padding: 10px 20px; background-color: red; color: white; text-decoration: none;'>Turn OFF</a>";

  html += "</body></html>";
  return html;
}

// Handle the root path "/"
void handleRoot() {
  server.send(200, "text/html", HTMLPage());
}

// Handle LED ON request
void handleLEDOn() {
  ledState = true;
  digitalWrite(ledPin, HIGH);
  server.send(200, "text/html", HTMLPage());
}

// Handle LED OFF request
void handleLEDOff() {
  ledState = false;
  digitalWrite(ledPin, LOW);
  server.send(200, "text/html", HTMLPage());
}
void handleLEDConnected(){
  ledState = false;
  digitalWrite(ledPin, RISING);
  delay(500);
  digitalWrite(ledPin, LOW);
  server.send(200, "text/html", HTMLPage());
}

//override from arduino.h?
void setup() {
  Serial.begin(115200); //SETS THIS UP TO SEND THE MESSAGE ALONG THE SERIAL BUS AT THIS RATE.

  // Initialize onboard LED pin as output
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW); // Start with LED off

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    // if(millis() - startTime > 30000){
    //   Serial.print("Please try to change your creds.");
    //   break;
    // }
  }
  if(WiFi.status() == WL_CONNECTED){
      handleLEDConnected();
  }
  Serial.println("\nConnected to Wi-Fi");
  Serial.println(WiFi.localIP());

  // Define server routes
  server.on("/", handleRoot);
  server.on("/on", handleLEDOn);
  server.on("/off", handleLEDOff);

  // Start the server
  server.begin();
  Serial.println("Server started");
}

//override from arduino.h
void loop() {
  server.handleClient(); // Handle client requests
  //do something to catch errors?
}
