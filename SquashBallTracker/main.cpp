﻿#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudaobjdetect.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudafilters.hpp>
#include <opencv2/video/tracking.hpp>
#include <opencv2/cudaarithm.hpp>
#include "Tracker.hpp"
#include "TrajectoryPreditor.hpp"

/*
Grouping rectangles that are near.
Near rectangles are within (width*groupSize)/2 and (height*groupSize)/2 of another rectangle
*/
 std::vector<cv::Rect> groupNearRects(std::vector<cv::Rect> rectangles, float groupSize) {  
	for (int i = 0; i < rectangles.size(); i++) {
		cv::Rect obj1 = rectangles[i];
		obj1.x -= (obj1.width / 2) * groupSize;
		obj1.y -= (obj1.height / 2) * groupSize;
		obj1.width += obj1.width * groupSize;
		obj1.height += obj1.height * groupSize * 2;

		for (int j = i + 1; j < rectangles.size(); j++) {
			cv::Rect obj2 = rectangles[j];
			obj2.x -= (obj2.width / 2) * groupSize;
			obj2.y -= (obj2.height / 2) * groupSize;
			obj2.width += obj2.width * groupSize;
			obj2.height += obj2.height * groupSize;

			if ((obj1 & obj2).area() != 0) {
				rectangles[i].x = MIN(obj1.x, obj2.x);
				rectangles[i].y = MIN(obj1.y, obj2.y);
				rectangles[i].width = obj1.br().x > obj2.br().x ? obj1.br().x - rectangles[i].x : obj2.br().x - rectangles[i].x;
				rectangles[i].height = obj1.br().y > obj2.br().y ? obj1.br().y - rectangles[i].y : obj2.br().y - rectangles[i].y;

				rectangles.erase(rectangles.begin() + j);
				j--;

			}
		}
	}
	return rectangles;
} 

int videoAnalysisV1() {
	//Adding path of video and capturing the frames using VideoCapture
	std::string videoPath = "E:\\Documents\\Projects\\opencv\\SquashFootage.avi";
	cv::VideoCapture cap(videoPath);

	cv::Mat cFr1, cFr2, cFr3;			//Capture Frames
	cv::Mat ball, person;				//Frame optimised for object detection

	cv::cuda::GpuMat d1, d2, d3;		//Delta Frames (Differences between capture frames)
	cv::cuda::GpuMat b1, b2, b3, bc;	//Frames optimised for ball tracking
	cv::cuda::GpuMat p1;				//Frames optimised for player tracking
	cv::cuda::GpuMat gFr1, gFr2, gFr3;	//GPU Frames

	//Adding the relevant CUDA method to manipulate each frame with the GPU
	cv::Ptr<cv::cuda::Filter> gausFilter = cv::cuda::createGaussianFilter(16, 16, cv::Size(3, 3), 0);

	cv::namedWindow("cFr1", cv::WINDOW_KEEPRATIO);

	int threshL = 5;
	int threshH = 255;

	int hLow = 54;
	int hMax = 128;
	int sLow = 160;
	int sMax = 237;
	int vLow = 0;
	int vMax = 144;

	cv::Scalar lower(hLow, sLow, vLow);
	cv::Scalar higher(hMax, sMax, vMax);

	std::vector<std::vector<cv::Point>> ballLocations, personLocations;
	std::vector<cv::Rect> possiblePeople, possibleBall;
	std::vector<cv::Rect> personIds, objIds;
	double area;
	bool intersect;

	//Creating the tracker and the variable type for object tracking between frames;
	sbt::SBTracker ballTracker;
	sbt::SBTracker shorbaggyIdentifier;
	sbt::SBTracker::TrackedObj t;

	std::vector<cv::Rect> foundProjectiles;
	while (true) {
		possibleBall.clear();
		possiblePeople.clear();

		//Capturing the frame and uploading it to the GPU
		cap >> cFr1;
		cap >> cFr2;
		cap >> cFr3;

		gFr1.upload(cFr1);
		gFr2.upload(cFr2);
		gFr3.upload(cFr3);

		//Adjusting the region of interest to ignore movement outside of the squash court
		cFr1.adjustROI(0, -50, -50, -50);
		gFr1.adjustROI(0, -50, -50, -50);
		gFr2.adjustROI(0, -50, -50, -50);
		gFr3.adjustROI(0, -50, -50, -50);

		//Bluring the image to reduce noise from the camera
		gausFilter->apply(gFr1, gFr1);
		gausFilter->apply(gFr2, gFr2);
		gausFilter->apply(gFr3, gFr3);

		//Processing the image to better show the movement of the players. Detecting the players from their HSV colour values
		cv::cuda::cvtColor(gFr1, p1, cv::COLOR_BGR2HSV);
		cv::cuda::inRange(p1, cv::Scalar(hLow, sLow, vLow), cv::Scalar(hMax, sMax, vMax), p1);

		//Calculating the deltas (Differences) between each frame. The motion of the ball can be found by finding the differences between consecutive frames
		cv::cuda::subtract(gFr2, gFr1, d1);
		cv::cuda::subtract(gFr3, gFr1, d2);
		cv::cuda::subtract(gFr3, gFr2, d3);

		//Processing the image to better show the movement of the ball
		cv::cuda::threshold(d1, b1, threshL, threshH, cv::THRESH_BINARY);
		cv::cuda::threshold(d2, b2, threshL, threshH, cv::THRESH_BINARY);
		cv::cuda::threshold(d3, b3, threshL, threshH, cv::THRESH_BINARY);
		cv::cuda::add(b1, b2, bc);
		cv::cuda::add(bc, b3, bc);
		cv::cuda::cvtColor(bc, bc, cv::COLOR_BGR2GRAY);

		//Converting the frames so they can be processed by the CPU again
		p1.download(person);
		bc.download(ball);

		//Finding the edges of the blobs in the images processed for the balls detection. Filtering these for objects whos area is between 50 and 300px
		cv::findContours(ball, ballLocations, cv::RETR_TREE, cv::CHAIN_APPROX_SIMPLE);
		for (size_t i = 0; i < ballLocations.size(); i++) {
			area = cv::contourArea(ballLocations[i]);
			if (area > 50 && area < 500) {
				possibleBall.push_back(boundingRect(ballLocations[i]));
			}
		}

		//Finding the edges in the images processed for the players detection.
		cv::findContours(person, personLocations, cv::RETR_TREE, cv::CHAIN_APPROX_SIMPLE);
		for (size_t i = 0; i < personLocations.size(); i++) {
			area = cv::contourArea(personLocations[i]);
			if (area > 20 && area < 10000) {
				possiblePeople.push_back(boundingRect(personLocations[i]));
			}
		}

		//Connecting near objects in the players frame. This "fills" the gaps between parts of the players that hasn't been found in the previous step.
		possiblePeople = groupNearRects(possiblePeople, 1);

		/*
		Removing all the objects detected by the findContours(Ball) function that lie within the players boundaries.
		These detections will be eronious ball detections. If the ball falls in the players boundary then it will be treated as occulded and need to be redected.
		*/
		for (int j = 0; j < possiblePeople.size(); j++) {
			for (int i = 0; i < possibleBall.size(); i++) {
				if ((possibleBall[i] & possiblePeople[j]).area() > 0) {
					possibleBall.erase(possibleBall.begin() + i);
					i--;
				}
			}
		}


		//Call the object Ball Tracker from the Squash Ball Tracker (SBTracker) class. Found in Tracker.cpp
		std::vector<std::vector<cv::Rect>> obj = ballTracker.distanceTracker(possibleBall);

		//Looping over the returned vectors of ball paths that have been found and drawing the path and bounding rectange for these objects
		for (int i = 0; i < obj.size(); i++) {
			for(int j = 1; j < obj[i].size(); j++) {
				cv::line(cFr1, cv::Point(obj[i][j - 1].x + (obj[i][j - 1].width / 2), obj[i][j - 1].y + (obj[i][j - 1].height / 2)), cv::Point(obj[i][j].x + (obj[i][j].width / 2), obj[i][j].y + (obj[i][j].height / 2)), cv::Scalar(i*46%255, i * 72%255, i * 113%255));
				cv::rectangle(cFr1, obj[i][j], cv::Scalar((100 * i) % 255, (25 * i ) % 255, (100 * i) % 255));
			}
		}

		//Drawing the bounding box for the players
		for (int i = 0; i < possiblePeople.size(); i++) {
				cv::rectangle(cFr1, possiblePeople[i], cv::Scalar(255, 0, 0), 3);
		}
		
		//Showing frame
		cv::imshow("cFr1", cFr1);
		
		//Method stops when key is pressed
		if (cv::waitKey(1) > 0) {
			break;
		}
	}
	cv::destroyAllWindows();
	return 0;
}

int main(int argc, char* argv[]) {
	videoAnalysisV1();
	return 0;
}