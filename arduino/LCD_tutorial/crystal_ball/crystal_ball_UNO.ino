include <LiquidCrystal.h>

//GLOBAL setup for vars -- LCD object selects pins for the screen
LiquidCrystal lcd(12, 11, 5,4,3,2);
const int switchPin = 6;  //we've go one pin for our switch that will be an in-place swap for a Gyroscope
int switchState = 0;
int previousSwitchState = 1;
int reply; 


// setup for connections
void setup(){
lcd.begin(16, 2) //16 columns, 12 rows
pinMode(switchPin, INPUT) //specifies control plane pin and what it does.

//start
lcd.print("Ask the")

lcd.setCursor(0,1) //zero--indexed for second line, oth row and 
lcd.print("crystal ball")
}



void loop(){
switchState = digitalRead(switchPin)

if(switchState != previousSwitchState){
  if(switchState == LOW){
    reply = random(8)
  }

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("The crystal ball says:")
  lcd.setCursor(0,1)
//basic switch for the reply. Subthis out for a function. 
  val strReply = switch(reply){
    //{insert switch values here}
  };

  previousSwitchState = switchState;

}

}

}


