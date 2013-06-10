#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>
#include <float.h>

#include "lsd_opencv.hpp"

#include "opencv2/opencv.hpp"
//core, imgproc

// LSD parameters 
#define SIGMA_SCALE 0.6    // Sigma for Gaussian filter is computed as sigma = sigma_scale/scale.
#define QUANT       2.0    // Bound to the quantization error on the gradient norm. 
#define ANG_TH      22.5   // Gradient angle tolerance in degrees.
#define LOG_EPS     0.0    // Detection threshold: -log10(NFA) > log_eps
#define DENSITY_TH  0.7    // Minimal density of region points in rectangle.
#define N_BINS      1024   // Number of bins in pseudo-ordering of gradient modulus.

// Other constants
// ln(10) 
#ifndef M_LN10
#define M_LN10 2.30258509299404568402
#endif // !M_LN10

// PI 
#ifndef M_PI
#define M_PI        CV_PI           // 3.14159265358979323846 
#endif
#define M_3_2_PI    (3*CV_PI) / 2   // 4.71238898038  // 3/2 pi 
#define M_2__PI     2*CV_PI         // 6.28318530718  // 2 pi 

// Label for pixels with undefined gradient. 
#define NOTDEF  (double)-1024.0

#define NOTUSED 0   // Label for pixels not used in yet. 
#define USED    1   // Label for pixels already used in detection. 

#define BIN_SIZE    1024

using namespace cv;

void LSD::flsd(const Mat& image, const double& scale, std::vector<Point2f>& begin, std::vector<Point2f>& end, 
    std::vector<double>& width, std::vector<double>& prec, std::vector<double>& nfa, Rect roi)
{
    //call the other method,
}

void LSD::flsd(const Mat& image, const double& scale, std::vector<lineSegment>& lines, Rect roi)
{
    CV_Assert(image.data != NULL);
    CV_Assert(scale > 0);

    // Angle tolerance
    double prec = M_PI * ANG_TH / 180.0;
    double p = ANG_TH / 180.0;
    double rho = QUANT / sin(prec);    // gradient magnitude threshold
 
    Mat angles(image.size(), CV_64F), modgrad(image.size(), CV_64F);
    vector<coorlist*> list;
    if (scale != 1)
    {
        //TODO: Remove Gaussian blur, as scaling down applies.
        Mat scaled_img, gaussian_img;
        double sigma = (scale < 1.0)?(SIGMA_SCALE / scale):(SIGMA_SCALE);
        double prec = 3.0;
        unsigned int h =  (unsigned int) ceil(sigma * sqrt(2.0 * prec * log(10.0)));
        int ksize = 1+2*h; // kernel size 
        // Create a Gaussian kernel
        Mat kernel = getGaussianKernel(ksize, sigma, CV_64F);
        // Apply to the image
        filter2D(image, gaussian_img, image.depth(), kernel, Point(-1, -1));
        // Scale image to needed size
        resize(gaussian_img, scaled_img, Size(), scale, scale);
        imshow("Gaussian image", scaled_img);
        ll_angle(scaled_img, rho, BIN_SIZE, angles, modgrad, list);
    }
    else
    {
        ll_angle(image, rho, BIN_SIZE, angles, modgrad, list);
    }
    
    double* q = (double*) angles.data;
    //memcpy(&q, angles.data, sizeof(double));
    std::cout << q[0] << std::endl;
    std::cout << "@CHECK: Angles >" << (double)angles.at<unsigned char>(0,0) << "<>" << 
        (double)angles.data[0] << "<"<< std::endl; // should be <double>(x,y) not <uchar>
    
    /* Number of Tests - NT
        The theoretical number of tests is Np.(XY)^(5/2)
        where X and Y are number of columns and rows of the image.
        Np corresponds to the number of angle precisions considered.
        As the procedure 'rect_improve' tests 5 times to halve the
        angle precision, and 5 more times after improving other factors,
        11 different precision values are potentially tested. Thus,
        the number of tests is
        11 * (X*Y)^(5/2)
        whose logarithm value is
        log10(11) + 5/2 * (log10(X) + log10(Y)).
    */
    int width = image.cols;
    int height = image.rows; 
    
    double logNT = 5.0 * (log10((double)width) + log10((double)height)) / 2.0 + log10(11.0);
    int min_reg_size = (int) (-logNT/log10(p)); /* minimal number of points in region that can give a meaningful event */

    Mat region = Mat::zeros(image.size(), CV_8UC1);
    Mat used = Mat::zeros(image.size(), CV_8UC1); // zeros = NOTUSED
    vector<cv::Point*> reg(width * height);

    // std::cout << "Search." << std::endl;
    // Search for line segments 
    int ls_count = 0;
    for(unsigned int i = 0; (i < list.size()) && list[i] != NULL; i++)
    {
        // std::cout << "Inside for 1: size " << list.size() << " image size: " << image.size() << std::endl;
        int adx = list[i]->p.x + list[i]->p.y * width;
        // std::cout << "adx " << adx << std::endl;
        // std::cout << "Used: " << used.data[adx] << std::endl;
        //if((used.data[adx] == NOTUSED) && (angles.data[adx] != NOTDEF))
        if((used.data[adx] == NOTUSED) && (angles.data[adx] != NOTDEF))
        {
            // std::cout << "Inside for 2 " << std::endl;
            int reg_size;
            region_grow();
            
            // Ignore small regions
            if(reg_size < min_reg_size) { continue; }

            // Construct rectangular approximation for the region
            region2rect();
            if(!refine()) { continue; }
            if(!refine()) { continue; }

            // Compute NFA
            double log_nfa = rect_improve();
            if(log_nfa <= LOG_EPS) { continue; }

            // Found new line
            ++ls_count;

        }
    
    }
 
}

void LSD::ll_angle(const cv::Mat& in, const double& threshold, const unsigned int& n_bins, 
                   cv::Mat& angles, cv::Mat& modgrad, std::vector<coorlist*>& list)
{
    angles = cv::Mat(in.size(), CV_64F); // Mat::zeros? to clean image
    modgrad = cv::Mat(in.size(), CV_64F);

    int width = in.cols;
    int height = in.rows;

    // Undefined the down and right boundaries 
    cv::Mat w_ndf(1, width, CV_64F, NOTDEF);
    cv::Mat h_ndf(height, 1, CV_64F, NOTDEF);
    w_ndf.row(0).copyTo(angles.row(height - 1));
    h_ndf.col(0).copyTo(angles.col(width -1));
    
    /* Computing gradient for remaining pixels */
    CV_Assert(in.isContinuous());   // Accessing image data linearly
    double max_grad = 0.0;
    for(int x = 0; x < width - 1; ++x)
    {
        for(int y = 0; y < height - 1; ++y)
        {
            /*
                Norm 2 computation using 2x2 pixel window:
                    A B
                    C D
                and
                    DA = D-A,  BC = B-C.
                Then
                    gx = B+D - (A+C)   horizontal difference
                    gy = C+D - (A+B)   vertical difference
                DA and BC are just to avoid 2 additions.
            */
            int adr = y * width + x; 
            double DA = in.data[adr + width + 1] - in.data[adr];
            double BC = in.data[adr + 1] - in.data[adr + width];
            double gx = DA + BC;    /* gradient x component */
            double gy = DA - BC;    /* gradient y component */
            double norm = std::sqrt((gx * gx + gy * gy)/4.0);   /* gradient norm */
            
            modgrad.data[adr] = norm;    /* store gradient*/

            if (norm <= threshold)  /* norm too small, gradient no defined */
            {
                angles.data[adr] = NOTDEF;
            }
            else
            {
                angles.data[adr] = std::atan2(gx, -gy);   /* gradient angle computation */
                if (norm > max_grad) { max_grad = norm; }
            }

        }
    }
    // std::cout << "Max grad: " << max_grad << std::endl;

    /* Compute histogram of gradient values */
    // // SLOW! 
    // std::vector<std::vector<cv::Point> > range(n_bins);
    // //for(int i = 0; i < n_bins; ++i) {range[i].reserve(width*height/n_bins); }
    // double bin_coef = (double) n_bins / max_grad;
    // for(int x = 0; x < width - 1; ++x)
    // {
    //     for(int y = 0; y < height - 1; ++y)
    //     {
    //         double norm = modgrad.data[y * width + x];
    //         /* store the point in the right bin according to its norm */
    //         int i = (unsigned int) (norm * bin_coef);
    //         range[i].push_back(cv::Point(x, y));
    //     }
    // }

    list = vector<coorlist*>(width * height, new coorlist());
    vector<coorlist*> range_s(n_bins, NULL);
    vector<coorlist*> range_e(n_bins, NULL);
    int count = 0;
    double bin_coef = (double) n_bins / max_grad;

    for(int x = 0; x < width - 1; ++x)
    {
        for(int y = 0; y < height - 1; ++y)
        {
            double norm = modgrad.data[y * width + x];
            /* store the point in the right bin according to its norm */
            int i = (unsigned int) (norm * bin_coef);
            //std::cout << "before assignment" << std::endl;
            if(range_e[i] == NULL)
            {
                // std::cout << "asdsad" << std::endl;
                range_e[i] = range_s[i] = list[count];
                ++count;
            }
            else
            {
                range_e[i]->next = list[count];
                range_e[i] = list[count];
                ++count;
            }
            //range_e[i] = new coorlist();
            //std::cout << "after assignment" << std::endl;
            range_e[i]->p = cv::Point(x, y);
            // std::cout << "between" << std::endl;
            range_e[i]->next = NULL;
            // std::cout << "loop end" << std::endl;
            
        }
    }

    // std::cout << "make list" << std::endl;
    // Sort
    int idx = n_bins - 1;
    for(;idx > 0 && range_s[idx] == NULL; idx--);
    coorlist* start = range_s[idx];
    coorlist* end = range_e[idx];
    if(start != NULL)
    {
        while(idx > 0)
        {
            --idx;
            if(range_s[idx] != NULL)
            {
                end->next = range_s[idx];
                end = range_e[idx];
            }
        }
    }

    // std::cout << "End" << std::endl;
    //imshow("Angles", angles);
}

void LSD::region_grow()
{

}

void LSD::region2rect()
{

}

bool LSD::refine()
{
    // test return
    return true;
}

double LSD::rect_improve()
{
    // test return
    return LOG_EPS;
}
