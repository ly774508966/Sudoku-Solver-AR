// Copyright 2017 James Bendig. See the COPYRIGHT file at the top-level
// directory of this distribution.
//
// Licensed under:
//   the MIT license
//     <LICENSE-MIT or https://opensource.org/licenses/MIT>
//   or the Apache License, Version 2.0
//     <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>,
// at your option. This file may not be copied, modified, or distributed
// except according to those terms.

#include <algorithm>
#include <iostream>
#include <tuple>
#include <cassert>
#include <cmath>
#ifdef __linux
#include <GLES3/gl3.h>
#elif defined _WIN32
#include <GL/glew.h>
#endif
#include <GLFW/glfw3.h>
#include "Camera.h"
#include "Geometry.h"
#include "Image.h"
#include "ImageProcessing.h"
#include "Painter.h"
#include "PuzzleFinder.h"

static constexpr unsigned int PUZZLE_IMAGE_WIDTH = 600;
static constexpr unsigned int PUZZLE_IMAGE_HEIGHT = PUZZLE_IMAGE_WIDTH;

static bool drawLines = false;
static bool drawLineClusters = false;
static bool drawPossiblePuzzleLineClusters = false;
static bool drawHoughTransform = false;

void CheckGLError()
{
	const GLenum error = glGetError();
	if(error == GL_NO_ERROR)
		return;

	std::cout << "OpenGL Error: " << error << std::endl;
	std::abort();
}

void DrawLines(Painter& painter,const float x,const float y,const float width,const float height,const std::vector<Line>& lines,const unsigned char red,const unsigned char green,const unsigned char blue)
{
	for(auto line : lines)
	{
		//Based on the equation x*cos(theta) + y*sin(theta) = rho.
		float theta = line.theta;
		float rho = line.rho;

		//Rho should be positive to simplify finding clipping points below.
		if(rho < 0.0f)
		{
			theta = fmodf(theta + M_PI,2.0f * M_PI);
			rho *= -1.0f;
		}

		//Get a point on the line. The actual line is 90 degrees from theta at this point.
		const float cosTheta = cosf(theta);
		const float sinTheta = sinf(theta);
		const float xPoint = cosTheta * rho;
		const float yPoint = sinTheta * rho;

		//Vertical line. Draw and get out early to avoid divide by zeroes below.
		if(sinTheta == 0.0f)
		{
			painter.DrawLine(x + xPoint,
							 y,
							 x + xPoint,
							 y + height,
							 red,green,blue);
			continue;
		}

		//Now to get the line equation: y = mx + b.
		//Let x1 = xPoint
		//    y1 = yPoint
		//    x2 = xPoint + cos(theta + PI/2)
		//    y2 = yPoint + sin(theta + PI/2).
		//Then m = (y2 - y1) / (x2 - x1) = sin(theta + PI/2) / cos(theta + Pi/2)
		//                               = tan(theta + PI/2)
		//                               = -(cosf(theta) / sinf(theta)).
		const float m = -(cosf(theta) / sinf(theta));
		const float b = -xPoint * m;


		//Spots where the line intersects the image.
		const float leftVertical = yPoint + b;
		const float topHorizontal = (-yPoint - b) / m;
		const float rightVertical = yPoint + b + width * m;
		const float bottomHorizontal = (height - yPoint - b) / m;

		//Clip line so that it fits in image.
		float x1 = 0.0f;
		float y1 = 0.0f;
		float x2 = 0.0f;
		float y2 = 0.0f;

		if(theta > 0.0f && theta <= M_PI/2) //Point is in the lower right quadrant.
		{
			x1 = leftVertical <= height ? 0.0f : bottomHorizontal;
			y1 = leftVertical <= height ? leftVertical : height;
			x2 = topHorizontal <= width ? topHorizontal : width;
			y2 = topHorizontal <= width ? 0.0f : rightVertical;
		}
		else if(theta >= M_PI/2 && theta <= M_PI) //Point is in the lower left quadrant.
		{
			if(leftVertical > height)
				continue; //Clip line, not within image.

			x1 = 0.0f;
			y1 = leftVertical;
			x2 = bottomHorizontal <= width ? bottomHorizontal : width;
			y2 = rightVertical <= height ? rightVertical : height;

		}
		else if(theta >= 3*M_PI/2) //Point is in the top right quadrant.
		{
			if(topHorizontal > width)
				continue; //Clip line, not within image.

			x1 = topHorizontal;
			y1 = 0.0f;
			x2 = bottomHorizontal <= width ? bottomHorizontal : width;
			y2 = bottomHorizontal <= width ? height : rightVertical;
		}
		//Line in the top left quadrant is never in the image.
		else
			continue;

		painter.DrawLine(x + x1,
						 y + y1,
						 x + x2,
						 y + y2,
						 red,green,blue);
	}
}

void DrawLineClusters(Painter& painter,const float x,const float y,const float width,const float height,const std::vector<std::vector<Line>>& lineClusters)
{
	//Random set of colors to alternate through so clusters can be told apart.
	std::vector<std::tuple<unsigned char,unsigned char,unsigned char>> clusterColors;
	clusterColors.push_back({255,0,0});
	clusterColors.push_back({128,0,255});
	clusterColors.push_back({0,255,0});
	clusterColors.push_back({255,255,0});
	clusterColors.push_back({0,255,255});
	clusterColors.push_back({128,255,255});
	clusterColors.push_back({255,0,255});

	for(unsigned int x = 0;x < lineClusters.size();x++)
	{
		const auto clusterColor = clusterColors[x % clusterColors.size()];
		const unsigned char red = std::get<0>(clusterColor);
		const unsigned char green = std::get<1>(clusterColor);
		const unsigned char blue = std::get<2>(clusterColor);
		DrawLines(painter,x,y,width,height,lineClusters[x],red,green,blue);
	}
}

void DrawHoughTransform(Painter& painter,const float windowWidth,const float windowHeight,const Image& houghTransformFrame,const float scale)
{
	//Find maximum hough transform value.
	unsigned short maximumValue = 0;
	for(unsigned int x = 0;x < houghTransformFrame.width * houghTransformFrame.height;x++)
	{
		const unsigned int index = x * 3;
		const unsigned short value = *reinterpret_cast<const unsigned short*>(&houghTransformFrame.data[index]);
		maximumValue = std::max(maximumValue,value);
	}

	Image modifiedHTF = houghTransformFrame;

	//Rescale hough transform into a 0-255 greyscale image so it can be displayed.
	const float multiplier = 255.0f / static_cast<float>(maximumValue);
	for(unsigned int x = 0;x < houghTransformFrame.width * houghTransformFrame.height;x++)
	{
		const unsigned int index = x * 3;
		const unsigned char value = static_cast<float>(*reinterpret_cast<const unsigned short*>(&houghTransformFrame.data[index])) * multiplier;

		modifiedHTF.data[index + 0] = value;
		modifiedHTF.data[index + 1] = value;
		modifiedHTF.data[index + 2] = value;
	}

	//Draw hough transform in the lower right corner of window.
	painter.DrawImage(windowWidth - houghTransformFrame.width * scale,
					  windowHeight - houghTransformFrame.height * scale,
					  houghTransformFrame.width * scale,
					  houghTransformFrame.height * scale,
					  modifiedHTF);
}

void FitImage(const unsigned int windowWidth,const unsigned int windowHeight,const Image& image,unsigned int& x,unsigned int& y,unsigned int& width,unsigned int& height)
{
	const float hRatio = static_cast<float>(image.width) / static_cast<float>(windowWidth);
	const float vRatio = static_cast<float>(image.height) / static_cast<float>(windowHeight);
	const float scale = 1.0f / std::max(hRatio,vRatio);

	width = image.width * scale;
	height = image.height * scale;
	x = abs(static_cast<int>(windowWidth) - static_cast<int>(width)) / 2;
	y = abs(static_cast<int>(windowHeight) - static_cast<int>(height)) / 2;
}

void OnKey(GLFWwindow* window,int key,int scancode,int action,int mode)
{
	if(action != GLFW_PRESS)
		return;

	if(key == GLFW_KEY_ESCAPE)
		glfwSetWindowShouldClose(window,GL_TRUE);
	else if(key == GLFW_KEY_0)
		drawHoughTransform = !drawHoughTransform;
	else if(key == GLFW_KEY_1)
		drawLines = !drawLines;
	else if(key == GLFW_KEY_2)
		drawLineClusters = !drawLineClusters;
	else if(key == GLFW_KEY_3)
		drawPossiblePuzzleLineClusters = !drawPossiblePuzzleLineClusters;
}

#ifdef __linux
int main(int argc,char* argv[])
#elif defined _WIN32
int __stdcall WinMain(void*,void*,void*,int)
#endif
{
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API,GLFW_OPENGL_ES_API);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,0);
	glfwWindowHint(GLFW_RESIZABLE,GL_FALSE);

	GLFWwindow* window = glfwCreateWindow(800 + PUZZLE_IMAGE_WIDTH,600,"Sudoku Solver AR",nullptr,nullptr);
	assert(window != nullptr);
	glfwMakeContextCurrent(window);
	glfwSetKeyCallback(window,OnKey);

#ifdef _WIN32
	glewInit();
#endif

	int windowWidth = 0;
	int windowHeight = 0;
	glfwGetFramebufferSize(window,&windowWidth,&windowHeight);

	Painter painter(windowWidth,windowHeight);
	Camera camera = Camera::Open("/dev/video0").value();
	Image frame;
	Image greyscaleFrame;
	Image cannyFrame;
	Canny canny = Canny::WithRadius(5.0f);
	Image mergedFrame;
	Image houghTransformFrame;
	Image puzzleFrame;
	PuzzleFinder puzzleFinder;

	while(!glfwWindowShouldClose(window))
	{
		glfwPollEvents();

		//Read frame.
		camera.CaptureFrameRGB(frame);

		//Figure out how to draw image so that it fits window.
		unsigned int drawImageX = 0;
		unsigned int drawImageY = 0;
		unsigned int drawImageWidth = 0;
		unsigned int drawImageHeight = 0;
		FitImage(windowWidth - PUZZLE_IMAGE_WIDTH,windowHeight,frame,drawImageX,drawImageY,drawImageWidth,drawImageHeight);

		//Process frame.
		RGBToGreyscale(frame,greyscaleFrame);
		canny.Process(greyscaleFrame,cannyFrame);
		BlendAdd(frame,cannyFrame,mergedFrame);

		HoughTransform(cannyFrame,houghTransformFrame);

		std::vector<Point> puzzlePoints;
		if(puzzleFinder.Find(drawImageWidth,drawImageHeight,houghTransformFrame,puzzlePoints))
			painter.ExtractImage(frame,puzzlePoints,1.0f / drawImageWidth,1.0f / drawImageHeight,puzzleFrame,PUZZLE_IMAGE_WIDTH,PUZZLE_IMAGE_HEIGHT);

		//Draw frame and extracted puzzle if available.
		glViewport(0,0,windowWidth,windowHeight);
		painter.DrawImage(drawImageX,drawImageY,drawImageWidth,drawImageHeight,mergedFrame);
		painter.DrawImage(800,0,PUZZLE_IMAGE_WIDTH,PUZZLE_IMAGE_HEIGHT,puzzleFrame);

		//Draw debug info.
		if(drawLines)
			DrawLines(painter,drawImageX,drawImageY,drawImageWidth,drawImageHeight,puzzleFinder.lines,10,10,10);
		if(drawLineClusters)
		{
			std::sort(puzzleFinder.lineClusters.begin(),puzzleFinder.lineClusters.end(),[](const auto& lhs,const auto& rhs) {
				return MeanTheta(lhs) < MeanTheta(rhs);
			});
			DrawLineClusters(painter,drawImageX,drawImageY,drawImageWidth,drawImageHeight,puzzleFinder.lineClusters);
		}
		if(drawPossiblePuzzleLineClusters)
		{
			std::sort(puzzleFinder.possiblePuzzleLineClusters.begin(),puzzleFinder.possiblePuzzleLineClusters.end(),[](const auto& lhs,const auto& rhs) {
				return MeanTheta(lhs) < MeanTheta(rhs);
			});
			DrawLineClusters(painter,drawImageX,drawImageY,drawImageWidth,drawImageHeight,puzzleFinder.possiblePuzzleLineClusters);
		}
		if(drawHoughTransform)
			DrawHoughTransform(painter,windowWidth - 600,windowHeight,houghTransformFrame,0.75f);

		CheckGLError();
		glfwSwapBuffers(window);
	}

	glfwTerminate();
	return 0;
}

