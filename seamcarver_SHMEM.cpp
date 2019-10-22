#include <pngwriter.h>
#include <lqr.h>
#include <getopt.h>
#include "liquidrescale.h"
#include <math.h>
#include <limits>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <thread>
#include <shmem.h>

using namespace std;
pngwriter pngwrt(1,1,0,"out_SHMEM.png");
int BASE_ENERGY = 1000;
int ROWS_PER_THREAD = 64;
int width = 0;
int height = 0;
gfloat rigidity = 0;
gint max_step = 1;
int channels = 3;
double** energyArray;
guchar* seams;
guchar* buffer;
int* verticalSeams;
double** distTo;
int** edgeTo;
guchar* carved_imageV;
pthread_barrier_t barr;
static long pSync[SHMEM_BARRIER_SYNC_SIZE];

struct ThreadData {
	int num_rows;
	int num_cols;
	int start_row;
	int stop_row;
	int thread_id;
	int thread_num;
	char* orientation;
};

double** initializeEnergyArray(int rows, int columns){
	double** energyArray;
	energyArray = (double**) shmem_malloc(rows*sizeof(double*));
	for (int i = 0; i < rows; i++)
		energyArray[i] = (double*) shmem_malloc(columns*sizeof(double));
	return energyArray;
}

double** initializeDistances(int rows, int columns)	{
	double** distTo;
	distTo = (double**) shmem_malloc(rows*sizeof(double*));
	for (int i = 0; i < rows; i++)
		distTo[i] = (double*) shmem_malloc(columns*sizeof(double));
	return distTo;
}

int** initializeEdges(int rows, int columns){
	int** edgeTo;
	edgeTo = (int**) shmem_malloc(rows*sizeof(int));
	for (int i = 0; i < rows; i++)
		edgeTo[i] = (int*) shmem_malloc(columns*sizeof(int));
	return edgeTo;
}
	
/*Copied from the liblqr example
  convert the image in the right format */
guchar * rgb_buffer_from_image(pngwriter *png)
{
	gint x, y, k, channels;
	gint w, h;
	guchar *buffer;

	/* get info from the image */
	w = png->getwidth();
	h = png->getheight();
	channels = 3;                       // we assume an RGB image here 

	/* allocate memory to store w * h * channels unsigned chars */
	buffer = g_try_new(guchar, channels * w * h);
	g_assert(buffer != NULL);

	/* start iteration (always y first, then x, then colours) */
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			for (k = 0; k < channels; k++) {
				/* read the image channel k at position x,y */
				buffer[(y * w + x) * channels + k] = (guchar) (png->dread(x + 1, y + 1, k + 1) * 255);
				/* note : the x+1,y+1,k+1 on the right side are
				 *        specific the pngwriter library */
			}
		}
	}

	return buffer;
}

double computeEnergy(int x, int y, guchar* buffer){
	if (x == 0 || y == 0 || (x == width - 1) || (y == height- 1))
		return BASE_ENERGY;

	//Declare values for the RGB top, bottom, left and right neighbours
	double top_RB, top_GB, top_BB = 0.0;
	double bottom_RB, bottom_GB, bottom_BB = 0.0;
	double left_RB, left_GB, left_BB = 0.0;
	double right_RB, right_GB, right_BB = 0.0;

	/*Declare values for the differences of the vertical and horizontal 
	  RGB neighbours difference*/
	double redDiffH, greenDiffH, blueDiffH = 0.0;
	double redDiffV, greenDiffV, blueDiffV = 0.0;

	/*Declare values for the sum of the vertical and horizontal RGB 	
	  neighbours differences*/
	double valueH, valueV,  valueSum;


	//Get the RGB values of the top neighbor of the current pixel

	top_RB = buffer[((y-1)*width+(x))*3+0];
	top_GB = buffer[((y-1)*width+(x))*3+1];
	top_BB = buffer[((y-1)*width+(x))*3+2];

	//Get the RGB values of the bottom neighbor of the current pixel
	bottom_RB = buffer[((y+1)*width+(x))*3+0];
	bottom_GB = buffer[((y+1)*width+(x))*3+1];
	bottom_BB = buffer[((y+1)*width+(x))*3+2];

	//Get the RGB values of the right neigbor of the current pixel
	right_RB = buffer[((y)*width+(x+1))*3+0];
	right_GB = buffer[((y)*width+(x+1))*3+1];
	right_BB = buffer[((y)*width+(x+1))*3+2];

	//Get the RGB values of the left neigbor of the current pixel
	left_RB = buffer[((y)*width+(x-1))*3+0];
	left_GB = buffer[((y)*width+(x-1))*3+1];
	left_BB = buffer[((y)*width+(x-1))*3+2];


	//Get the absolute difference of the red horizontal neighbours
	redDiffV  = abs(top_RB - bottom_RB);

	//Get the absolute difference of the red horizontal neighbours
	redDiffH  = abs(right_RB - left_RB);

	//Get the absolute difference of the green horizontal neighbours
	greenDiffV  = abs(top_GB - bottom_GB);

	//Get the absolute difference of the green horizontal neighbours
	greenDiffH  = abs(right_GB - left_GB);

	//Get the absolute difference of the blue horizontal neighbours
	blueDiffV  = abs(top_BB - bottom_BB);

	//Get the absolute difference of the blue horizontal neighbours
	blueDiffH  = abs(right_BB - left_BB);

	//Get ths sum of the square of differences of vertical neighbours
	valueH = pow(redDiffH,2) + pow(greenDiffH,2) + pow(blueDiffH,2);

	//Get ths sum of the square of differences of horizontal neighbours
	valueV = pow(redDiffV,2) + pow(greenDiffV,2) + pow(blueDiffV,2);
	valueSum = valueV + valueH;

	//Return the squareroot of the sum of differences
	return sqrt(valueSum);
}

//Generate energyArray 
void generateEnergyMatrix(int width, int height, char* orientation){
	//Declare a dynamic 2D array to hold the energy values for all pixels
	for (int row = 1; row < height; row++)    {
		for(int column= 0; column < width; column++)    {
			if (orientation[0] == 'v')
				energyArray[row][column] = computeEnergy(column, row, buffer);
			else
				energyArray[row][column] = computeEnergy(row, column, buffer);
		}
	}

}

/*Declare a relax function to optimize the computation of a 
  shortest path energy values*/

void relax(int row, int col, int** edgeTo, double** distTo, int width) {
	int relax = 0;
	int nextRow = row + 1;
	for (int i = -1; i <= 1; i++) {
		int nextCol = col + i;
		if (nextCol < 0 || nextCol >= width)
			continue;
		if (distTo[nextRow][nextCol] >= distTo[row][col] + energyArray[nextRow][nextCol]) {
			distTo[nextRow][nextCol] = distTo[row][col] + energyArray[nextRow][nextCol];
			edgeTo[nextRow][nextCol] = i;
		}
	}
}

//Backtrack to identify seams which is the shortest path across the energy array
int* backTrack(int** edgeTo, double** distTo, int height, int width){
	// Backtrack from the last row to get a shortest path
	int* seams = new int[height];
	int minCol = 0;
	double minDist = std::numeric_limits<double>::infinity();
	for (int col = 0; col < width; col++) {
		if (distTo[height - 1][col] < minDist) {
			minDist = distTo[height - 1][col];
			minCol = col;
		}
	}
	for (int row = height - 1; row >= 0; row--) {
		verticalSeams[row] = minCol;
		minCol = minCol - edgeTo[row][minCol];
	}
	return verticalSeams;

}

//A Transpose Matrix that makes findVerticalSeams re-usable for horizontal seams
guchar*  transposeRGBuffer(guchar* buffer, int width, int height) {
	guchar* transposedRGBuffer;
	int size = 3 * width * height;
	transposedRGBuffer = g_try_new(guchar,size);
	g_assert(transposedRGBuffer != NULL);
	for(int col = 0; col < width; col++){
		for(int row = 0; row < height; row++){
			for (int color = 0; color < 3; color++){
				transposedRGBuffer[(col*height + row)*3 + color] = buffer[(row*width+col)*3+color];
			}			
		}
	}
	return transposedRGBuffer;
}

void generateEnergyMatrix(int start_row, int stop_row, int num_pes, char* orientation){
	 for (int row = 1; row < stop_row; row++){
                for (int column = 0; column < width; column++){
                        if (orientation[0] == 'v')
                                energyArray[row][column] = computeEnergy(column, row, buffer);
                        else
                                energyArray[row][column] = computeEnergy(row, column, buffer);
			double current_val = energyArray[row][column];
                        shmem_double_put(&energyArray[row][column], &current_val, 1, 0);
                }
        }
	shmem_barrier_all();
        if (num_pes > 1)
                shmem_broadcast32(energyArray, energyArray, height * width, 0, 0, 0, num_pes, pSync);
}
//Identify the seams in an image
int* identifySeams( int width, int height){

	for (int i = 0; i < height; i++)
		distTo[i] = new double[width];

	//Declare an array to hold the paths taken to reach a pixel
	for (int i = 0; i < height; i++)
		edgeTo[i] = new int[width];

	//Initialize distTo to maximum values

	for (int row = 0; row < height; row++) {
		for (int col = 0; col < width; col++) {
			if (row == 0)
				distTo[row][col] = BASE_ENERGY;                                                              else                                                                                                         distTo[row][col] = std::numeric_limits<double>::infinity();
		}
	}                                                                                                    for (int row = 0; row < height - 1; row++) {
		for (int col = 0; col < width; col++) {
			relax(row, col, edgeTo, distTo, width);                                                      }                                                                                            }
}                                                                                 //Carve out the seams from the image               
guchar* carveSeams(int start_row, int stop_row)	{
	for (int row = 0; row < height; row++){
                //Get the RGB value before the seam
                for (int col = 0; col < verticalSeams[row]; col++){
                        for (int color = 0; color < 3; color++){
                                carved_imageV[(row * width + col) * 3 + color] = buffer[(row * width
                                                + col) * 3 + color];
                                seams[(row * width + col) * 3 + color] = buffer[(row * width + col) *
                                        3 + color];
                        }
                }

               // Get the RGB values after the seams
                for (int col = verticalSeams[row]; col < width-1; col++) {
                        for (int color = 0; color < 3; color++){
                                carved_imageV[(row * width + col) * 3 + color] = buffer[(row * width
                                                + col+1) * 3 + color];
                                seams[(row * width + col+1) * 3 + color] = buffer[(row * width + col+
                                                1) * 3 + color];
                        }
                }
        }

        return carved_imageV;
}

guchar* carveSeams(int* vertical_seams, guchar* buffer, int width, int height){
        guchar* carved_imageV;
        int size = 3 * width * height;
        // allocate memory to store w * h * channels unsigned chars
        carved_imageV = g_try_new(guchar, size);
        g_assert(carved_imageV != NULL);
        seams = g_try_new(guchar, size);
        g_assert(seams != NULL);


        for (int row = 0; row < height; row++){
               // Get the RGB value before the seam
                for (int col = 0; col < vertical_seams[row]; col++){
                        for (int color = 0; color < 3; color++){
                                carved_imageV[(row * width + col) * 3 + color] = buffer[(row * width + col) * 3 + color]
;
                                seams[(row * width + col) * 3 + color] = buffer[(row * width + col) * 3 + color];
                        }
                }

                //Get the RGB values after the seams
                for (int col = vertical_seams[row]; col < width-1; col++) {
                        for (int color = 0; color < 3; color++){
                                carved_imageV[(row * width + col) * 3 + color] = buffer[(row * width + col+1) * 3 + color];
                                seams[(row * width + col+1) * 3 + color] = buffer[(row * width + col+1) * 3 + color];
                        }
                }
        }

        return carved_imageV;
}
LqrRetVal printSeams(LqrCarver *carver, pngwriter *pngwrt){

	gint x, y;
	guchar *rgb;
	gdouble red, green, blue;

	lqr_carver_scan_reset(carver);

	/* readout (no need to init rgb) */
	while (lqr_carver_scan(carver, &x, &y, &rgb)) {
		/* convert the output into doubles */
		red = (gdouble) rgb[0] / 255;
		green = (gdouble) rgb[1] / 255;
		blue = (gdouble) rgb[2] / 255;

		if(!rgb[0] && !rgb[1] && !rgb[2])
			pngwrt->plot(x + 1, y + 1, 1.0, 0.0, 0.0);
		else 
			pngwrt->plot(x + 1, y + 1, red, green, blue);
	}
	return LQR_OK;
}

LqrRetVal write_carver_to_image(LqrCarver *carver, pngwriter *pngwrt, char* orientation){

	gint x, y;
	guchar *rgb;
	gdouble red, green, blue;

	/* resize the image canvas as needed to
	   fit for the new size 
	   Resize based on the orientation*/

	if(orientation[0] == 'v'){
		TRAP(lqr_carver_resize(carver, width-1 , height));
		pngwrt->resize(width-1, height);
	}
	else{
		TRAP(lqr_carver_resize(carver, width , height-1));
		pngwrt->resize(width,height-1);
	}
	lqr_carver_scan_reset(carver);


	/* readout (no need to init rgb) */
	while (lqr_carver_scan(carver, &x, &y, &rgb)) {
		/* convert the output into doubles */
		red = (gdouble) rgb[0] / 255;
		green = (gdouble) rgb[1] / 255;
		blue = (gdouble) rgb[2] / 255;

		/* plot (pngwriter's coordinates start from 1,1) */
		pngwrt->plot(x + 1, y + 1, red, green, blue);
	}
	return LQR_OK;
}

/* Get current time*/

double timestamp()
{
	struct timeval tval;

	gettimeofday( &tval, ( struct timezone * ) 0 );
	return ( tval.tv_sec + (tval.tv_usec / 1000000.0) );
}

int main(int argc, char **argv){
	shmem_init();
	int me = shmem_my_pe();
	int npes = shmem_n_pes();

	char * original_img = argv[1]; 
	char* orientation = argv[2];
	double begin, end;
	begin = timestamp();
	pngwrt.readfromfile(original_img);
	width = pngwrt.getwidth();
	height = pngwrt.getheight();
	for (int i = 0; i < SHMEM_BARRIER_SYNC_SIZE; i++) 
		pSync[i] = SHMEM_SYNC_VALUE;
        shmem_sync_all();

        if (me == 0){
		cout<<"Width: "<<width<<" Height: "<<height<<endl;
	}
		
	int size = 3 * width * height;
	buffer = g_try_new(guchar,size);
	buffer = rgb_buffer_from_image(&pngwrt);

	g_assert(buffer != NULL);
	carved_imageV = g_try_new(guchar, size);
	g_assert(carved_imageV != NULL);
	seams = g_try_new(guchar, size);
	g_assert(seams != NULL);

	LqrCarver *carver;
	LqrCarver *carved_seams;
	int* v_seams;
	//Check the orientation to determine how to carve
	//Vertical Orientation
	if(orientation[0] == 'v'){
		verticalSeams = new int[height];
		distTo = initializeDistances(height, width);
		edgeTo = initializeEdges(height, width);
		energyArray = initializeEnergyArray(height, width);
		//Divide the work among the PEs
		int row_per_pe = (height + npes - 1)/npes;
	
		int start_row = me * row_per_pe;
		int stop_row = (me + 1)*row_per_pe; 
		
		//Ensure the last PE does not go past the height
		if (me == npes - 1)
			stop_row = height;

		generateEnergyMatrix(start_row, stop_row, npes, orientation);
		if (me == 0){
			cout<<"Removing vertical seams"<<endl;
		//	Seams Identification
			identifySeams(width, height);
			verticalSeams =  backTrack(edgeTo, distTo, height, width);
		//Carve out the identified seams
		guchar* carved_imageV = carveSeams(verticalSeams,buffer, width, height);
		cout<<"After carving seams out "<<endl;
		carver = lqr_carver_new(carved_imageV, width, height, 3);
		carved_seams = lqr_carver_new(seams, width, height, 3);
	}
}
	//Horizontal Seams
	else{
		verticalSeams = new int[width];
                distTo = initializeDistances(width, height);
                edgeTo = initializeEdges(width, height);
                energyArray = initializeEnergyArray(width, height);
		//cout << "After all initialization"<<endl;
                //Divide the work among the PEs
                int row_per_pe = (width + npes - 1)/npes;

                int start_row = me * row_per_pe;
                int stop_row = (me + 1)*row_per_pe;

                //Ensure the last PE does not go past the height
                if (me == npes - 1)
                        stop_row = width;
		//cout << "Before generating energy"<<endl;
                generateEnergyMatrix(start_row, stop_row, npes, orientation);
                if (me == 0){
                        cout<<"Removing horizontal seams"<<endl;
                        //Seams Identification
                cout<<"After generating energy"<<endl;
		identifySeams(width, height);
                verticalSeams =  backTrack(edgeTo, distTo, width, height);
		buffer = transposeRGBuffer(buffer, width, height);
                //carve out the identified seams
                guchar* carved_imageV = carveSeams(verticalSeams,buffer, height, width);
                  carver = lqr_carver_new(transposeRGBuffer(carved_imageV, height,width), width, height, 3);
                carved_seams = lqr_carver_new(transposeRGBuffer(seams, height,width), width, height, 3);
		}
		/*verticalSeams = new int[width];
		distTo = new double*[width];
		edgeTo = new int*[width];

		//Declare a dynamic 2D array to hold the energy values for all pixels
		energyArray = new double*[width];
		int num_threads = width/64;
		pthread_t threads[num_threads];
		struct ThreadData data[num_threads];
		int row_per_thread = (width + num_threads - 1)/num_threads;
		//Spawn up threads that will generate the energy matrix
		for (int i = 0; i < num_threads;  i++){
			data[i].start_row = i * row_per_thread;
			data[i].stop_row = (i + 1)*row_per_thread;
			data[i].num_rows = width;
			data[i].num_cols = height;
			data[i].thread_id = i;
			data[i].thread_num = num_threads;
			data[i].orientation = orientation;
		}
		data[0].start_row = 0;
		data[num_threads-1].stop_row = width;
		for (int i = 0; i <num_threads; i++){
			pthread_create(&threads[i], NULL, &generateEnergyMatrix, (void*)&data[i]);
		}
		for (int i = 0; i < num_threads; i++){
			pthread_join(threads[i], NULL);
		}

		cout<<"Removing horizontal seams"<<endl;
		identifySeams(height, width);
		verticalSeams =  backTrack(edgeTo, distTo, width, height);

		buffer = transposeRGBuffer(buffer, width, height);

		for (int i = 0; i < num_threads; i++){
			pthread_create(&threads[i], NULL, &carveSeams, (void*)&data[i]);
		}

		for (int i = 0;  i < num_threads; i++){
			pthread_join(threads[i], NULL);
		}

		carver = lqr_carver_new(transposeRGBuffer(carved_imageV, height,width), width, height, 3);
		carved_seams = lqr_carver_new(transposeRGBuffer(seams, height,width), width, height, 3);*/
	}

	//Create a Carver object with the carved image buffer
	if (me == 0){
		TRAP(lqr_carver_init(carver, max_step, rigidity));
		//write_carver_to_image(carver, &pngwrt, orientation);
		printSeams(carved_seams, &pngwrt);
		lqr_carver_destroy(carver);
		pngwrt.close();
		double end = timestamp();
		printf("%s%5.2f\n","TOTAL TIME: ", (end-begin));
	}
	shmem_finalize();
	return 0;
}

