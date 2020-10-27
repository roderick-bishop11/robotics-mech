
/*
Program that computes inverse kinematics of a theta-theta manipulator. When given angles, the program computes actuator positions to reach said angles. 

*/

import java.util.Scanner;
public class InverseKinematics
{// begin class
    public static void main(String args[ ])
    {//begin main  method

		Scanner inp = new Scanner(System.in);

	// convert strings to numbers
	double x = inp.nextDouble();
	double y = inp.nextDouble();

	// given links length
	double a1 = 0.3;
	double a2 = 0.3;

	// compute inverse kinematics
	double theta2 = Math.acos(  (x*x + y*y - a1*a1-a2*a2) / (2*a1*a2) );
	double theta1 = Math.atan (y/x)  -  
			Math.atan (  (a2*Math.sin(theta2)) / (a1+a2* Math.cos(theta2)) );

	// print the result
 	System.out.println ("Given desired gripper position x = "+x+", y = "+y);
	System.out.println(" The motors should be rotated as :");
	System.out.println(" theta1 = " + theta1);
	System.out.println(" Theta2 = " + theta2);

    }// end main method
}// end class
