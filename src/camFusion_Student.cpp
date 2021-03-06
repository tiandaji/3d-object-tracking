
#include <iostream>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <set>
#include <iterator>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "camFusion.hpp"
#include "dataStructures.h"

using namespace std;


// Create groups of Lidar points whose projection into the camera falls into the same bounding box
void clusterLidarWithROI(std::vector<BoundingBox> &boundingBoxes, std::vector<LidarPoint> &lidarPoints, float shrinkFactor, cv::Mat &P_rect_xx, cv::Mat &R_rect_xx, cv::Mat &RT)
{
    // loop over all Lidar points and associate them to a 2D bounding box
    cv::Mat X(4, 1, cv::DataType<double>::type);
    cv::Mat Y(3, 1, cv::DataType<double>::type);

    for (auto it1 = lidarPoints.begin(); it1 != lidarPoints.end(); ++it1)
    {
        // assemble vector for matrix-vector-multiplication
        X.at<double>(0, 0) = it1->x;
        X.at<double>(1, 0) = it1->y;
        X.at<double>(2, 0) = it1->z;
        X.at<double>(3, 0) = 1;

        // project Lidar point into camera
        Y = P_rect_xx * R_rect_xx * RT * X;
        cv::Point pt;
        pt.x = Y.at<double>(0, 0) / Y.at<double>(0, 2); // pixel coordinates
        pt.y = Y.at<double>(1, 0) / Y.at<double>(0, 2);

        vector<vector<BoundingBox>::iterator> enclosingBoxes; // pointers to all bounding boxes which enclose the current Lidar point
        for (vector<BoundingBox>::iterator it2 = boundingBoxes.begin(); it2 != boundingBoxes.end(); ++it2)
        {
            // shrink current bounding box slightly to avoid having too many outlier points around the edges
            cv::Rect smallerBox;
            smallerBox.x = (*it2).roi.x + shrinkFactor * (*it2).roi.width / 2.0;
            smallerBox.y = (*it2).roi.y + shrinkFactor * (*it2).roi.height / 2.0;
            smallerBox.width = (*it2).roi.width * (1 - shrinkFactor);
            smallerBox.height = (*it2).roi.height * (1 - shrinkFactor);

            // check wether point is within current bounding box
            if (smallerBox.contains(pt))
            {
                enclosingBoxes.push_back(it2);
            }

        } // eof loop over all bounding boxes

        // check wether point has been enclosed by one or by multiple boxes
        if (enclosingBoxes.size() == 1)
        { 
            // add Lidar point to bounding box
            enclosingBoxes[0]->lidarPoints.push_back(*it1);
        }

    } // eof loop over all Lidar points
}


void show3DObjects(std::vector<BoundingBox> &boundingBoxes, cv::Size worldSize, cv::Size imageSize, bool bWait)
{
    // create topview image
    cv::Mat topviewImg(imageSize, CV_8UC3, cv::Scalar(255, 255, 255));

    for(auto it1=boundingBoxes.begin(); it1!=boundingBoxes.end(); ++it1)
    {
        // create randomized color for current 3D object
        cv::RNG rng(it1->boxID);
        cv::Scalar currColor = cv::Scalar(rng.uniform(0,150), rng.uniform(0, 150), rng.uniform(0, 150));

        // plot Lidar points into top view image
        int top=1e8, left=1e8, bottom=0.0, right=0.0; 
        float xwmin=1e8, ywmin=1e8, ywmax=-1e8;
        for (auto it2 = it1->lidarPoints.begin(); it2 != it1->lidarPoints.end(); ++it2)
        {
            // world coordinates
            float xw = (*it2).x; // world position in m with x facing forward from sensor
            float yw = (*it2).y; // world position in m with y facing left from sensor
            xwmin = xwmin<xw ? xwmin : xw;
            ywmin = ywmin<yw ? ywmin : yw;
            ywmax = ywmax>yw ? ywmax : yw;

            // top-view coordinates
            int y = (-xw * imageSize.height / worldSize.height) + imageSize.height;
            int x = (-yw * imageSize.width / worldSize.width) + imageSize.width / 2;

            // find enclosing rectangle
            top = top<y ? top : y;
            left = left<x ? left : x;
            bottom = bottom>y ? bottom : y;
            right = right>x ? right : x;

            // draw individual point
            cv::circle(topviewImg, cv::Point(x, y), 4, currColor, -1);
        }

        // draw enclosing rectangle
        cv::rectangle(topviewImg, cv::Point(left, top), cv::Point(right, bottom),cv::Scalar(0,0,0), 2);

        // augment object with some key data
        char str1[200], str2[200];
        sprintf(str1, "id=%d, #pts=%d", it1->boxID, (int)it1->lidarPoints.size());
        putText(topviewImg, str1, cv::Point2f(left-250, bottom+50), cv::FONT_ITALIC, 2, currColor);
        sprintf(str2, "xmin=%2.2f m, yw=%2.2f m", xwmin, ywmax-ywmin);
        putText(topviewImg, str2, cv::Point2f(left-250, bottom+125), cv::FONT_ITALIC, 2, currColor);  
    }

    // plot distance markers
    float lineSpacing = 2.0; // gap between distance markers
    int nMarkers = floor(worldSize.height / lineSpacing);
    for (int i = 0; i < nMarkers; ++i)
    {
        int y = (-(i * lineSpacing) * imageSize.height / worldSize.height) + imageSize.height;
        cv::line(topviewImg, cv::Point(0, y), cv::Point(imageSize.width, y), cv::Scalar(255, 0, 0));
    }

    // display image
    string windowName = "3D Objects";
    cv::namedWindow(windowName, 1);
    cv::imshow(windowName, topviewImg);

    if(bWait)
    {
        cv::waitKey(0); // wait for key to be pressed
    }
}

// associate a given bounding box with the keypoints it contains
void clusterKptMatchesWithROI(BoundingBox &boundingBox, std::vector<cv::KeyPoint> &kptsPrev,
                              std::vector<cv::KeyPoint> &kptsCurr, std::vector<cv::DMatch> &kptMatches)
{
    double filterOutliersRatio = 0.2; // remove 20% of the most high valued distances
    std::multiset<double> euclideanDistances;

    // calculate Euclidean distances between keypoints
    for (const auto& match : kptMatches)
    {
        const auto &currKpt = kptsCurr[match.trainIdx];

        if (boundingBox.roi.contains(currKpt.pt))
        {
            const auto &prevKpt = kptsPrev[match.queryIdx];

            euclideanDistances.emplace(cv::norm(currKpt.pt - prevKpt.pt));
        }
    }
    const double euclideanDistanceMean =
            std::accumulate(euclideanDistances.begin(), euclideanDistances.end(), 0.0) / euclideanDistances.size();
    const double euclideanDistanceStandardDeviation = std::sqrt(
            std::accumulate(euclideanDistances.begin(), euclideanDistances.end(), 0.0,
                            [&euclideanDistanceMean](const double sum, const double dist)
                            {
                                double deviation = dist - euclideanDistanceMean;
                                return sum + deviation * deviation;
                            }) / euclideanDistances.size());

    auto r_offset = static_cast<size_t>(round(filterOutliersRatio * euclideanDistances.size()));
    auto it = euclideanDistances.crend();
    std::advance(it, r_offset);
    double filterKptsWithDistHigherThan = *it;

    for (const auto& match : kptMatches)
    {
        const auto& currKpt = kptsCurr[match.trainIdx];

        if (boundingBox.roi.contains(currKpt.pt))
        {
            const auto& prevKpt = kptsPrev[match.queryIdx];
            const double euclideanDistance = cv::norm(currKpt.pt - prevKpt.pt);
            if (euclideanDistance <= filterKptsWithDistHigherThan)
            {
                boundingBox.keypoints.push_back(currKpt);
                boundingBox.kptMatches.push_back(match);
            }
        }
    }

    std::cout << "[clusterKptMatchesWithROI]: points in ROI for BB "
              << boundingBox.boxID << " before filtering: " << euclideanDistances.size()
              << "; after filtering: " << boundingBox.keypoints.size()
              << "; Euclidean Distance Mean: " << euclideanDistanceMean
              << "; Euclidean distance standard deviation: " << euclideanDistanceStandardDeviation
              << std::endl;
}


// Compute time-to-collision (TTC) based on keypoint correspondences in successive images
void computeTTCCamera(std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, 
                      std::vector<cv::DMatch> kptMatches, double frameRate, double &TTC)
{
    // compute distance ratios between all matched keypoints
    vector<double> distRatios; // stores the distance ratios for all keypoints between curr. and prev. frame
    for (auto it1 = kptMatches.begin(); it1 != kptMatches.end() - 1; ++it1)
    { // outer kpt. loop

        // get current keypoint and its matched partner in the prev. frame
        cv::KeyPoint kpOuterCurr = kptsCurr.at(it1->trainIdx);
        cv::KeyPoint kpOuterPrev = kptsPrev.at(it1->queryIdx);

        for (auto it2 = kptMatches.begin() + 1; it2 != kptMatches.end(); ++it2)
        { // inner kpt.-loop

            double minDist = 100.0; // min. required distance

            // get next keypoint and its matched partner in the prev. frame
            cv::KeyPoint kpInnerCurr = kptsCurr.at(it2->trainIdx);
            cv::KeyPoint kpInnerPrev = kptsPrev.at(it2->queryIdx);

            // compute distances and distance ratios
            double distCurr = cv::norm(kpOuterCurr.pt - kpInnerCurr.pt);
            double distPrev = cv::norm(kpOuterPrev.pt - kpInnerPrev.pt);

            if (distPrev > std::numeric_limits<double>::epsilon() && distCurr >= minDist)
            { // avoid division by zero
                double distRatio = distCurr / distPrev;
                distRatios.push_back(distRatio);
            }
        } // eof inner loop over all matched kpts
    }     // eof outer loop over all matched kpts

    // only continue if list of distance ratios is not empty
    if (distRatios.empty())
    {
        TTC = NAN;
        return;
    }

    std::sort(distRatios.begin(), distRatios.end());
    long medIndex = floor(distRatios.size() / 2.0);
    // compute median dist. ratio to remove outlier influence
    double medDistRatio = distRatios.size() % 2 == 0 ?
            (distRatios[medIndex - 1] + distRatios[medIndex]) / 2.0 : distRatios[medIndex];

    double dT = 1 / frameRate;
    TTC = -dT / (1 - medDistRatio);
}


void computeTTCLidar(std::vector<LidarPoint> &lidarPointsPrev,
                     std::vector<LidarPoint> &lidarPointsCurr, double frameRate, double &TTC)
{
    // make a min-heap out of lidar points in-place with respect to the X coordinate;
    // we want to extract K minimum elements from arrays of lidar points to compute statistically
    // reliable distance to the preceding vehicle;
    // we can achieve O(N + K*log(N)) time complexity with the heap in comparison to sorting (O(N*log(N))),
    // where N is the number of lidar points and K << N (significantly less than)

    const size_t K = 13;
    const size_t P = 5;
    assert (K <= lidarPointsPrev.size() and K <= lidarPointsCurr.size());

    // we use > with respect to x coordinate because std::make_heap makes a max heap w.r.t. less operator
    auto cmpFunc = [](const LidarPoint& lp1, const LidarPoint& lp2) { return lp1.x > lp2.x; };

    std::make_heap(lidarPointsPrev.begin(), lidarPointsPrev.end(), cmpFunc);
    // the first K elements in the array will be the K minimum distance elements w.r.t. X coordinates
    // sorted in the descending order, that is, the Kth closes element will be the first one and the closest one
    // will be on the Kth position (index K-1)
    std::sort_heap(lidarPointsPrev.begin(), lidarPointsPrev.begin()+K, cmpFunc);

    std::make_heap(lidarPointsCurr.begin(), lidarPointsCurr.end(), cmpFunc);
    std::sort_heap(lidarPointsCurr.begin(), lidarPointsCurr.begin()+K, cmpFunc);

    // take average of (K-P)th to Kth point to compute the distance to the preceding vehicle
    auto sumOp = [](const double sum, const LidarPoint& lp) { return sum + lp.x; };
    double prevMeanX = std::accumulate(lidarPointsPrev.begin(), lidarPointsPrev.begin()+K, 0.0, sumOp) / P;
    double currMeanX = std::accumulate(lidarPointsCurr.begin(), lidarPointsCurr.begin()+K, 0.0, sumOp) / P;

    // compute TTC in accordance with the constant velocity motion model
    double T = 1.0 / frameRate;
    TTC = currMeanX * T / (prevMeanX - currMeanX);
}

static std::vector<size_t>
findBoundingBoxesContainingKeypoint(const cv::KeyPoint &kpt, const std::vector<BoundingBox> &bounding_boxes)
{
    std::vector<size_t> filtered_bounding_boxes;
    for (size_t i = 0; i < bounding_boxes.size(); ++i)
    {
        if (bounding_boxes[i].roi.contains(kpt.pt))
        {
            filtered_bounding_boxes.push_back(i);
        }
    }

    return filtered_bounding_boxes;
}


void matchBoundingBoxes(std::vector<cv::DMatch> &matches, std::map<int, int> &bbBestMatches,
                        DataFrame &prevFrame, DataFrame &currFrame)
{
    const size_t prevBBSize = prevFrame.boundingBoxes.size();
    const size_t currBBSize = currFrame.boundingBoxes.size();

    // 2D array storing the number of matched keypoints belonging
    // to the given bounding boxes from the previous frame and the current frame;
    // all elements are initialized to zero here
    std::vector<std::vector<size_t>> cntKptsInMatchedBB(prevBBSize, std::vector<size_t>(currBBSize, 0ull));

    // matches array contains all the matched keypoints between the previous and current frames
    for (const auto& match : matches)
    {
        // extract the keypoints for the corresponding match
        const auto& prevKpt = prevFrame.keypoints[match.queryIdx];
        const auto& currKpt = currFrame.keypoints[match.trainIdx];

        // find bounding boxes in the previous frame to which the given prevKpt belongs to
        const auto prevBoxIDs = findBoundingBoxesContainingKeypoint(prevKpt, prevFrame.boundingBoxes);
        const auto currBoxIDs = findBoundingBoxesContainingKeypoint(currKpt, currFrame.boundingBoxes);

        // update the number of matched keypoints from previous and current frames in the cntKptsInMatchedBB
        for (auto prevBBID : prevBoxIDs)
        {
            for (auto currBBID : currBoxIDs)
            {
                ++cntKptsInMatchedBB[prevBBID][currBBID];
            }
        }
    }

    // search for the maximum number of matches within bounding boxes;
    // there are no more correspondences between bounding boxes of previous and current frames than the minimum
    // number of bounding boxes among those frames;
    // since there should be no big difference in the number of identified bounding boxes in the previous
    // and current frames, we extract indices of maximum elements for both cases
    std::vector<size_t> max_prev_indices(currBBSize, 0ull);   // all elements are initialized to zero here
    std::vector<size_t> max_curr_indices(prevBBSize, 0ull);   //

    for (size_t prev_ind = 0; prev_ind < prevBBSize; ++prev_ind)
    {
        for (size_t curr_ind = 0; curr_ind < currBBSize; ++curr_ind)
        {
            max_prev_indices[curr_ind] =
                    cntKptsInMatchedBB[prev_ind][curr_ind] > cntKptsInMatchedBB[max_prev_indices[curr_ind]][curr_ind]
                    ? prev_ind : max_prev_indices[curr_ind];

            max_curr_indices[prev_ind] =
                    cntKptsInMatchedBB[prev_ind][curr_ind] > cntKptsInMatchedBB[prev_ind][max_curr_indices[prev_ind]]
                    ? curr_ind : max_curr_indices[prev_ind];
        }
    }

    // fill in the bbBestMatches based on the minimum number of bounding boxes because otherwise there might be
    // that one BB corresponds to two or more other
    if (prevBBSize <= currBBSize)
    {
        for (size_t curr_ind = 0; curr_ind < currBBSize; ++curr_ind)
        {
            auto prevBoxID = prevFrame.boundingBoxes[max_prev_indices[curr_ind]].boxID;
            auto currBoxID = currFrame.boundingBoxes[curr_ind].boxID;

            bbBestMatches[prevBoxID] = currBoxID;

            // the logic of this project assumes that the following asserts are true;
            // it may not be true if similar logic is applied in some other project
            assert (static_cast<size_t>(prevBoxID) == max_prev_indices[curr_ind]);
            assert (static_cast<size_t>(currBoxID) == curr_ind);
        }
    }
    else
    {
        for (size_t prev_ind = 0; prev_ind < prevBBSize; ++prev_ind)
        {
            auto prevBoxID = prevFrame.boundingBoxes[prev_ind].boxID;
            auto currBoxID = currFrame.boundingBoxes[max_curr_indices[prev_ind]].boxID;

            bbBestMatches[prevBoxID] = currBoxID;

            // the logic of this project assumes that the following asserts are true;
            // it may not be true if similar logic is applied in some other project
            assert (static_cast<size_t>(prevBoxID) == prev_ind);
            assert (static_cast<size_t>(currBoxID) == max_curr_indices[prev_ind]);
        }
    }

}
