#include "patch.h"

using namespace PAIS;

/* static functions */
bool Patch::isNeighbor(const Patch &pth1, const Patch &pth2) {
	const Vec3d &n1 = pth1.getNormal();
	const Vec3d &n2 = pth2.getNormal();
	if (n1.ddot(n2) > 0.99) {
		return true;
	}
	return false;
	/*
	const Vec3d &c1 = pth1.getCenter();
	const Vec3d &c2 = pth2.getCenter();
	const Vec3d &n1 = pth1.getNormal();
	const Vec3d &n2 = pth2.getNormal();

	double dist = 0;
	dist += abs((c1-c2).ddot(n1));
	dist += abs((c1-c2).ddot(n2));

	if (dist < 0.01) {
		return true;
	} else { 
		return false;
	}
	*/
}

/* constructor */
Patch::Patch(const Vec3d &center, const Vec3b &color, const vector<int> &camIdx, const vector<Vec2d> &imgPoint, const int id) : AbstractPatch(id) {
    this->center   = center;
    this->color    = color;
    this->camIdx   = camIdx;
    this->imgPoint = imgPoint;
	setEstimatedNormal();
}

Patch::Patch(const Vec3d &center, const Patch &parent, const int id) : AbstractPatch(id) {
    this->center    = center;
	this->camIdx    = parent.getCameraIndices();
	setNormal(parent.getNormal());
	expandVisibleCamera();
}

Patch::Patch(const Vec3d &center, const Vec2d &normalS, const vector<int> &camIdx, const double fitness, const double correlation, const int id) : AbstractPatch(id) {
	this->center = center;
	this->camIdx = camIdx;
	this->fitness = fitness;
	this->correlation = correlation;
	setNormal(normalS);
	setReferenceCameraIndex();
	setDepthAndRay();
	setDepthRange();
	setLOD();
	setPriority();
	setImagePoint();
}

Patch::~Patch(void) {

}

/* public functions */

void Patch::refine() {
	const MVS &mvs = MVS::getInstance();

	setReferenceCameraIndex();
	setDepthAndRay();
	setDepthRange();
	setLOD();

	// PSO parameter range (theta, phi, depth)
    double rangeL [] = {0.0 , normalS[1] - M_PI/2.0, depthRange[0]};
    double rangeU [] = {M_PI, normalS[1] + M_PI/2.0, depthRange[1]};

    // initial guess particle
    double init   [] = {normalS[0], normalS[1], depth};

	PsoSolver solver(3, rangeL, rangeU, PAIS::getFitness, this, mvs.maxIteration, mvs.particleNum);
    solver.setParticle(init);
    solver.run(true);

	// set refined patch information
	fitness = solver.getGbestFitness();
    const double *gBest = solver.getGbest();
    setNormal(Vec2d(gBest[0], gBest[1]));
    depth  = gBest[2];
	center = ray * depth + mvs.getCamera(refCamIdx).getCenter();

	return;

	/*
	int beforeRefCamIdx = refCamIdx;
	setReferenceCameraIndex();
	if (beforeRefCamIdx != refCamIdx) {
		refine();
	}

    int beforeCamNum = getCameraNumber();
	// set normalized homography patch correlation table
    setCorrelationTable();
	// remove insvisible camera
    removeInvisibleCamera();
    int afterCamNum = getCameraNumber();
	if (beforeCamNum != afterCamNum && afterCamNum >= mvs.minCamNum) {
		refine();
    }
	if (beforeCamNum == afterCamNum && afterCamNum >= mvs.minCamNum) {
		// set patch priority
		setPriority();
		// set image point
		setImagePoint();
	}

	*/
}

/* process */

void Patch::getHomographies(const Vec3d &center, const Vec3d &normal, vector<Mat_<double>> &H) const {
	const MVS &mvs = MVS::getInstance();
	const vector<Camera> &cameras = mvs.getCameras();

	// camera parameters
	const Camera &refCam  = mvs.getCamera(refCamIdx);
	const Mat_<double> &KRF = refCam.getKR();
	const Mat_<double> &KTF = refCam.getKT();

	// plane equation (distance form plane to origin)
	const double d = -center.ddot(normal);          
	const Mat_<double> normalM(normal);

	// set container
	const int camNum = getCameraNumber();
	H.resize(camNum);

	// LOD scalar for camera intrisic matrix K
	Mat_<double> LODM = Mat_<double>::zeros(3, 3);
	LODM.at<double>(0, 0) = 1.0/(1<<LOD);
	LODM.at<double>(1, 1) = 1.0/(1<<LOD);
	LODM.at<double>(2, 2) = 1.0;

	// get homography from reference to target image
	Mat_<double> invH = ( d*LODM*KRF - LODM*KTF*normalM.t() ).inv();
	for (int i = 0; i < camNum; i++) {
		// indentity for reference camera
		if (camIdx[i] == refCamIdx) {
			H[i] = Mat_<double>::eye(3, 3);
			continue;
		}

		// visible camera
		const Camera &cam = cameras[camIdx[i]];

		// get homography matrix
		const Mat_<double> &KRT = cam.getKR();        // K*R of to camera
		const Mat_<double> &KTT = cam.getKT();        // K*T of to camera
		H[i] = ( d*LODM*KRT - LODM*KTT*normalM.t() ) * invH;
	}
}

void Patch::getHomographyPatch(const Vec2d &pt, const Mat_<uchar> &img, const Mat_<double> &H, Mat_<double> &hp) const {
	const MVS &mvs = MVS::getInstance();
	const int patchRadius = mvs.patchRadius;
	const int patchSize   = mvs.patchSize;

	hp = Mat_<double>(patchSize*patchSize, 1);

	double w, ix, iy;                // position on target image
	int px[4];                       // neighbor x
	int py[4];                       // neighbor y
	int count = 0;
	double sum = 0;
	for (double x = pt[0]-patchRadius; x <= pt[0]+patchRadius; ++x) {
		for (double y = pt[1]-patchRadius; y <= pt[1]+patchRadius; ++y) {
			// homography projection (with LOD transform)
			w  = ( H.at<double>(2, 0) * x + H.at<double>(2, 1) * y + H.at<double>(2, 2) );
			ix = ( H.at<double>(0, 0) * x + H.at<double>(0, 1) * y + H.at<double>(0, 2) ) / w;
			iy = ( H.at<double>(1, 0) * x + H.at<double>(1, 1) * y + H.at<double>(1, 2) ) / w;

			// skip overflow cases
			if (ix < 0 || ix >= img.cols-1 || iy < 0 || iy >= img.rows-1 || w == 0) {
				hp = Mat_<double>::ones(patchSize*patchSize, 1) * DBL_MAX;
				printf("ID %d \t correlation overflow %f \t %f\n", getId(), ix, iy);
				return;
			}

			// interpolation neighbor points
			px[0] = (int) ix;
			py[0] = (int) iy;
			px[1] = px[0] + 1;
			py[1] = py[0];
			px[2] = px[0];
			py[2] = py[0] + 1;
			px[3] = px[0] + 1;
			py[3] = py[0] + 1;
				
			hp.at<double>(count, 0) = (double) img.at<uchar>(py[0], px[0])*(px[1]-ix)*(py[2]-iy) + 
					                  (double) img.at<uchar>(py[1], px[1])*(ix-px[0])*(py[2]-iy) + 
					                  (double) img.at<uchar>(py[2], px[2])*(px[1]-ix)*(iy-py[0]) + 
					                  (double) img.at<uchar>(py[3], px[3])*(ix-px[0])*(iy-py[0]);

			sum += hp.at<double>(count, 0)*hp.at<double>(count, 0);
			++count;
		}
	}

	hp /= sqrt(sum);
}

/* setters */

void Patch::setEstimatedNormal() {
	const MVS &mvs = MVS::getInstance();
    const int camNum = getCameraNumber();

    Vec3d dir;
    normal = Vec3d(0.0, 0.0, 0.0);
    for (int i = 0; i < camNum; i++) {
        const Camera &cam = mvs.getCamera(camIdx[i]);
        dir = cam.getCenter() - center;
        dir *= (1.0 / norm(dir));
        normal += dir;
    }
    normal *= (1.0 / norm(normal));
        
    setNormal(normal);
}

void Patch::setReferenceCameraIndex() {
	const MVS &mvs = MVS::getInstance();
    const int camNum = getCameraNumber();

    refCamIdx = -1;
    double maxCorr = -DBL_MAX;
    double corr;
    for (int i = 0; i < camNum; i++) {
        const Camera &cam = mvs.getCamera(camIdx[i]);
        corr = normal.ddot(-cam.getOpticalNormal());

        if (corr > maxCorr) {
            maxCorr = corr;
            refCamIdx = camIdx[i];
        }
    }
}

void Patch::setDepthAndRay() {
    ray = center - MVS::getInstance().getCamera(refCamIdx).getCenter();
    depth = norm(ray);
    ray = ray * (1.0 / depth);
}

void Patch::setDepthRange() {
	const MVS &mvs   = MVS::getInstance();
    const int camNum = getCameraNumber();
    const Camera &refCam = mvs.getCamera(refCamIdx);

	const double cellSize = mvs.cellSize;

    // center shift
    Vec3d c2 = ray * (depth+1.0) + refCam.getCenter();
    // projected point
    Vec2d p1, p2;

    double worldDist;
    double maxWorldDist = -DBL_MAX;
    for (int i = 0; i < camNum; i++) {
        if (camIdx[i] == refCamIdx) continue;
                
        const Camera &cam = mvs.getCamera(camIdx[i]);

        cam.project(center, p1);
		cam.project(c2, p2);

        worldDist = 1.0 / norm(p1-p2);

        if (worldDist > maxWorldDist) {
            maxWorldDist = worldDist;
        }
    }

	depthRange[0] = depth - max(cellSize, 5.0) * maxWorldDist;
    depthRange[1] = depth + max(cellSize, 5.0) * maxWorldDist;
}

void Patch::setLOD() {
	if (refCamIdx < 0) {
        printf("Reference camera index not set\n");
        return;
    }

	const MVS &mvs = MVS::getInstance();

    // patch size
	int patchRadius = mvs.patchRadius;
    int size        = mvs.patchSize;

    // reference camera
	const Camera &refCam = mvs.getCamera(refCamIdx);
    // image pyramid of reference image
    const vector<Mat_<uchar> > &pyramid = refCam.getPyramidImage();

    // texture info
    double mean = 0;
    double variance = 0;
    int count;

    // textures in window
    uchar *textures = new uchar[size*size];
        
    // projected point on image
    Vec2d pt;
        
    // initialize level of detail
	LOD = mvs.minLOD-1;

    // find LOD
	while (variance < mvs.textureVariation) {
        // goto next LOD
        LOD++;

        // return if reach the max LOD
        if (LOD >= pyramid.size()) {
            delete [] textures;
            return;
        }

        // LOD-- if out of image bound
        if ( !refCam.project(center, pt, LOD) ) {
            //printf("setLOD image point out of image bound: LOD %d, x: %f, y: %f\n", LOD, pt[0], pt[1]);
            LOD = max(LOD-1, 0);
            delete [] textures;
            return;
        }

        // clear
        mean     = 0;
        variance = 0;
        count    = 0;

        // get mean
        for (int x = cvRound(pt[0])-patchRadius; x <= cvRound(pt[0])+patchRadius; x++) {
            for (int y = cvRound(pt[1])-patchRadius; y <= cvRound(pt[1])+patchRadius; y++) {
                if ( !refCam.inImage(x, y, LOD) ) {
                    //printf("setLOD image point out of image bound: LOD %d, x: %d, y: %d\n", LOD, x, y);
                    LOD = max(LOD-1, 0);
                    delete [] textures;
                    return;
                }
                textures[count] = pyramid[LOD].at<uchar>(y, x);
                mean += textures[count];
                count++;
            }
        }
        mean /= count;

        // get variance
        for (int i = 0; i < count; i++) {
            variance += (textures[i]-mean)*(textures[i]-mean);
        }
        variance /= count; 
    }

    /* show pyramid */
    /*
    if (LOD > 0) {
        char title[30];
        for (int l = 0; l <= LOD; l++) {
            refCam.project(center, pt, l);
            sprintf(title, "LOD %d", l);
            Mat img = pyramid[l].clone();
            circle(img, Point(cvRound(pt[0]), cvRound(pt[1])), max(patchRadius, 5), Scalar(255,255,255), 2, CV_AA);
            imshow(title, img);
            cvMoveWindow(title, 0, 0);
        }
        waitKey();
        destroyAllWindows();
    }
    */
    delete [] textures;
}

void Patch::setCorrelationTable() {
	const MVS &mvs = MVS::getInstance();
	const vector<Camera> &cameras = mvs.cameras;

	// camera parameters
	const int camNum        = getCameraNumber();
	const Camera &refCam    = mvs.getCamera(refCamIdx);

	vector<Mat_<double>> H;
	getHomographies(center, normal, H);

	// 2D image point on reference image
	Vec2d pt;
	refCam.project(center, pt, LOD);

	// get normalized homography patch column vector
	vector<Mat_<double> > HP(camNum);
	#pragma omp parallel for
	for (int i = 0; i < camNum; i++) {
		const Mat_<uchar> &img = cameras[camIdx[i]].getPyramidImage(LOD);
		getHomographyPatch(pt, img, H[i], HP[i]);
	}

	// correlation table
	corrTable = Mat_<double>(camNum, camNum);
	for (int i = 0; i < camNum; ++i) {
		corrTable.at<double>(i, i) = 0;
		for (int j = i+1; j < camNum; ++j) {
			Mat_<double> corr = HP[i].t() * HP[j];
			corrTable.at<double>(i, j) = corr.at<double>(0, 0);
			corrTable.at<double>(j, i) = corrTable.at<double>(i, j);
		}
	}

	// set average correlation
	correlation = 0;
	for (int i = 0; i < corrTable.rows; ++i) {
		for (int j = 0; j < corrTable.cols; ++j) {
			correlation += corrTable.at<double>(i,j);
		}
	}
	correlation /= (camNum*camNum-camNum);
}

void Patch::setPriority() {
	const MVS &mvs = MVS::getInstance();
	const int totalCamNum = (int) mvs.getCameras().size();
	const int camNum = getCameraNumber();
	double camRatio = 1.0 - ((double) camNum) / ((double) totalCamNum+1);
	priority = fitness * exp(-correlation) * camRatio;
}

void Patch::setImagePoint() {
	const MVS &mvs                = MVS::getInstance();
	const int camNum              = getCameraNumber();
	const vector<Camera> &cameras = mvs.getCameras();
	const Camera &refCam          = cameras[refCamIdx];
	const Mat_<Vec3b> &img        = refCam.getRgbImage();

	// set image points
	imgPoint.resize(camNum);
	for (int i = 0; i < camNum; ++i) {
		const Camera &cam = cameras[camIdx[i]];
		cam.project(center, imgPoint[i]);
	}

	// set point color
	Vec2d pt;
	if ( refCam.project(center, pt) ) {
		color = img.at<Vec3b>(cvRound(pt[1]), cvRound(pt[0]));
	}
}

void Patch::removeInvisibleCamera() {
	const MVS &mvs = MVS::getInstance();
	const int camNum = getCameraNumber();

	// sum correlation and find max correlation index
	double corrSum;
	double maxCorr = -DBL_MAX;
	int maxIdx = 0;
	for (int i = 0; i < camNum; ++i) {
		corrSum = 0;
		for (int j = 0; j < camNum; ++j) {
			corrSum += corrTable.at<double>(i, j);
		}

		if (corrSum >= maxCorr) {
			maxIdx = i;
			maxCorr = corrSum;
		}
	}

	// remove invisible camera
	vector<int> removeIdx;
	// mark idx
	for (int i = 0; i < camNum; ++i) {
		if (i == maxIdx) continue;
		if (corrTable.at<double>(maxIdx, i) < mvs.minCorrelation) {
			removeIdx.push_back(camIdx[i]);
		}
	}
	// remove idx
	vector<int>::iterator it;
	for (int i = 0; i < (int) removeIdx.size(); i++) {
		it = find(camIdx.begin(), camIdx.end(), removeIdx[i]);
		camIdx.erase(it);
	}
}

void Patch::expandVisibleCamera() {
	const MVS &mvs = MVS::getInstance();
	const vector<Camera> &cameras = mvs.cameras;

	vector<int> expCamIdx;
	for (int i = 0; i < cameras.size(); ++i) {
		const Camera &cam = cameras[i];
		if (normal.ddot(-cam.getOpticalNormal()) >= mvs.visibleCorrelation) {
			expCamIdx.push_back(i);
		}
	}

	// use parent visible camera when not enough visible cameras
	if (expCamIdx.size() < mvs.minCamNum) {

		for (int i = 0; i < camIdx.size(); ++i) {
			const Camera &cam = cameras[camIdx[i]];
			if (normal.ddot(-cam.getOpticalNormal()) >= mvs.visibleCorrelation/2.0) {
				expCamIdx.push_back(camIdx[i]);
			}
		}
		
		sort(expCamIdx.begin(), expCamIdx.end());
		vector<int>::iterator it = unique(expCamIdx.begin(), expCamIdx.end());
		expCamIdx.resize(it - expCamIdx.begin());
	}

	camIdx = expCamIdx;
}

/* misc */
void Patch::showRefinedResult() const {
	const MVS &mvs = MVS::getInstance();
	const vector<Camera> &cameras = mvs.getCameras();
	const int patchRadius = mvs.getPatchRadius();

	// camera parameters
	const int camNum        = getCameraNumber();
	const Camera &refCam    = mvs.getCamera(refCamIdx);
	const Mat_<double> &KRF = refCam.getKR();
	const Mat_<double> &KTF = refCam.getKT();
	
	// plane equation (distance form plane to origin)
	const double d = -center.ddot(normal);          
	const Mat_<double> normalM(normal);

	// LOD scalar for camera intrisic matrix
	Mat_<double> LODM = Mat_<double>::zeros(3, 3);
	LODM.at<double>(0, 0) = 1.0/(1<<LOD);
	LODM.at<double>(1, 1) = 1.0/(1<<LOD);
	LODM.at<double>(2, 2) = 1.0;

	// Homographies to visible camera
	vector<Mat_<double> > H(camNum);
	for (int i = 0; i < camNum; i++) {
		// visible camera
		const Camera &cam = cameras[camIdx[i]];

		// get homography matrix
		const Mat_<double> &KRT = cam.getKR();        // K*R of to camera
		const Mat_<double> &KTT = cam.getKT();        // K*T of to camera
		H[i] = ( d*LODM*KRT - LODM*KTT*normalM.t() ) * ( d*LODM*KRF - LODM*KTF*normalM.t() ).inv();
	}

	Vec2d pt;
	refCam.project(center, pt, LOD);
	
	double x[5] = {pt[0]-patchRadius, pt[0]-patchRadius, pt[0]+patchRadius, pt[0]+patchRadius, pt[0]};
	double y[5] = {pt[1]-patchRadius, pt[1]+patchRadius, pt[1]-patchRadius, pt[1]+patchRadius, pt[1]};

	double w, ix[5], iy[5];
	char title[30];
	for (int i = 0; i < camNum; i++) {
		const Camera &cam = mvs.getCameras()[camIdx[i]];
		Mat_<Vec3b> img = cam.getRgbImage().clone();
		resize(img, img, Size(img.cols/(1<<LOD), img.rows/(1<<LOD) ) );

		// homography projection
		Mat F = Utility::getFundamental(refCam, cam);
		for (int c = 0; c < 5; c++) {
			w  =   H[i].at<double>(2, 0) * x[c] + H[i].at<double>(2, 1) * y[c] + H[i].at<double>(2, 2);
			ix[c] = ( H[i].at<double>(0, 0) * x[c] + H[i].at<double>(0, 1) * y[c] + H[i].at<double>(0, 2) ) / w;
			iy[c] = ( H[i].at<double>(1, 0) * x[c] + H[i].at<double>(1, 1) * y[c] + H[i].at<double>(1, 2) ) / w;
			ix[c] = cvRound(ix[c]);
			iy[c] = cvRound(iy[c]);

			// draw epipolar line
			/*
			Mat_<double> pF(3, 1);
			pF.at<double>(0,0) = x[c];
			pF.at<double>(1,0) = y[c];
			pF.at<double>(2,0) = 1;
			Mat epiLine = F*pF;
			double yStart = -epiLine.at<double>(0, 2)/epiLine.at<double>(0, 1);
			double yEnd   = (-epiLine.at<double>(0, 2)-epiLine.at<double>(0, 0)*img.cols) / epiLine.at<double>(0, 1);
			line(img, Point(0, cvRound(yStart)), Point( img.cols, cvRound(yEnd)), Scalar(0,255,0, 0.5));
			*/
		}

		// draw patch
		line(img, Point(ix[0],iy[0]), Point(ix[1],iy[1]), Scalar(0,0,255));
		line(img, Point(ix[0],iy[0]), Point(ix[2],iy[2]), Scalar(0,0,255));
		line(img, Point(ix[3],iy[3]), Point(ix[1],iy[1]), Scalar(0,0,255));
		line(img, Point(ix[3],iy[3]), Point(ix[2],iy[2]), Scalar(0,0,255));
		circle(img, Point(ix[4], iy[4]), 1, Scalar(0,0,255));

		sprintf(title, "img%d_%d.png", getId(), camIdx[i]);
		if (camIdx[i] == refCamIdx) {
			sprintf(title, "reference img%d_%d.png", getId(), camIdx[i]);
		}
		imshow(title, img);
		//imwrite(title, img);
	}
}

void Patch::showError() const {
	const MVS &mvs = MVS::getInstance();
	const vector<Camera> &cameras = mvs.getCameras();
	const int patchRadius = mvs.getPatchRadius();
	const int patchSize   = mvs.getPatchSize();

	// camera parameters
	const Camera &refCam  = mvs.getCamera(refCamIdx);
	const int camNum = getCameraNumber();

	// Homographies to visible camera
	vector<Mat_<double> > H(camNum);
	getHomographies(center, normal, H);

	Vec2d pt;
	refCam.project(center, pt, LOD);

	// warping (get pixel-wised variance)
	double mean, avgSad;             // pixel-wised mean, avgSad
	double w, ix, iy;                // position on target image
	int px[4];                       // neighbor x
	int py[4];                       // neighbor y
	double *c = new double [camNum]; // bilinear color
	Mat_<double> error(patchSize, patchSize);

	for (double x = pt[0]-patchRadius, ex = 0; x <= pt[0]+patchRadius; x++, ex++) {
		for (double y = pt[1]-patchRadius, ey = 0; y <= pt[1]+patchRadius; y++, ey++) {
			// clear
			mean = 0;
			avgSad = 0;

			for (int i = 0; i < camNum; i++) {
				const Mat_<uchar> &img = cameras[camIdx[i]].getPyramidImage(LOD);

				// homography projection
				w  =   H[i].at<double>(2, 0) * x + H[i].at<double>(2, 1) * y + H[i].at<double>(2, 2);
				ix = ( H[i].at<double>(0, 0) * x + H[i].at<double>(0, 1) * y + H[i].at<double>(0, 2) ) / w;
				iy = ( H[i].at<double>(1, 0) * x + H[i].at<double>(1, 1) * y + H[i].at<double>(1, 2) ) / w;
				
				// apply LOD transform
				if (LOD > 0) {
					ix /= 1<<LOD;
					iy /= 1<<LOD;
				}
				
				// interpolation neighbor points
				px[0] = (int) ix;
				py[0] = (int) iy;
				px[1] = px[0] + 1;
				py[1] = py[0];
				px[2] = px[0];
				py[2] = py[0] + 1;
				px[3] = px[0] + 1;
				py[3] = py[0] + 1;

				c[i] = (double) img.at<uchar>(py[0], px[0])*(px[1]-ix)*(py[2]-iy) + 
					   (double) img.at<uchar>(py[1], px[1])*(ix-px[0])*(py[3]-iy) + 
					   (double) img.at<uchar>(py[2], px[2])*(iy-py[0])*(px[3]-ix) + 
					   (double) img.at<uchar>(py[3], px[3])*(ix-px[2])*(iy-py[1]);

				mean += c[i];
			} // end of camera

			mean /= camNum;

			for (int i = 0; i < camNum; i++) {
				avgSad += abs(c[i]-mean);
			}
			avgSad /= camNum;

			error.at<double>(ey, ex) = avgSad;
		} // end of warping y
	} // end of warping x

	double minE ,maxE;
	minMaxLoc(error, &minE, &maxE);
	error = (error - minE) / (maxE - minE);
	char title[30];
	sprintf(title, "error%d.png", getId());
	resize(error, error, Size(200, 200), 0, 0, CV_INTER_NN);
	imshow(title, error);
}

/* fitness function */

double PAIS::getFitness(const Particle &p, void *obj) {
	// MVS
	const MVS &mvs                = MVS::getInstance();
	const int patchRadius         = mvs.getPatchRadius();
	const vector<Camera> &cameras = mvs.getCameras();

	// current patch
	const Patch  &patch   = *((Patch *)obj);
	// visible camera indices
	const vector<int> &camIdx = patch.getCameraIndices();
	// level of detail
	int LOD = patch.getLOD();

	// camera parameters
	const Camera &refCam  = mvs.getCamera(patch.getReferenceCameraIndex());
	const int camNum = patch.getCameraNumber();

	// given patch normal
	Vec3d normal;
	Utility::spherical2Normal(Vec2d(p.pos[0], p.pos[1]), normal);

	// skip inversed normal
	if (normal.ddot(refCam.getOpticalNormal()) > 0) {
        return DBL_MAX;
    }

	// given patch center
	const Vec3d center = patch.getRay() * p.pos[2] + refCam.getCenter();

	// Homographies to visible camera
	vector<Mat_<double> > H(camNum);
	patch.getHomographies(center, normal, H);

	// projected point on reference image with LOD transform
	Vec2d pt;
	if ( !refCam.project(center, pt, LOD) ) {
		return DBL_MAX;
	}

	// warping (get pixel-wised variance)
	double mean, avgSad;             // pixel-wised mean, average sad
	double w, ix, iy;                // position on target image
	int px[4];                       // neighbor x
	int py[4];                       // neighbor y
	double *c = new double [camNum]; // bilinear color
	double fitness = 0;              // result of normalized fitness
	// distance weighting
	Mat_<double>::const_iterator it = mvs.getPatchDistanceWeighting().begin();

	for (double x = pt[0]-patchRadius; x <= pt[0]+patchRadius; ++x) {
		for (double y = pt[1]-patchRadius; y <= pt[1]+patchRadius; ++y) {
			// clear
			mean   = 0;
			avgSad = 0;

			for (int i = 0; i < camNum; ++i) {
				const Mat_<uchar> &img = cameras[camIdx[i]].getPyramidImage(LOD);

				// homography projection
				w  = ( H[i].at<double>(2, 0) * x + H[i].at<double>(2, 1) * y + H[i].at<double>(2, 2) );
				ix = ( H[i].at<double>(0, 0) * x + H[i].at<double>(0, 1) * y + H[i].at<double>(0, 2) ) / w;
				iy = ( H[i].at<double>(1, 0) * x + H[i].at<double>(1, 1) * y + H[i].at<double>(1, 2) ) / w;

				// skip overflow cases
				if (ix < 0 || ix >= img.cols-1 || iy < 0 || iy >= img.rows-1 || w == 0) {
					delete [] c;
					return DBL_MAX;
				}
				
				// interpolation neighbor points
				px[0] = (int) ix;
				py[0] = (int) iy;
				px[1] = px[0] + 1;
				py[1] = py[0];
				px[2] = px[0];
				py[2] = py[0] + 1;
				px[3] = px[0] + 1;
				py[3] = py[0] + 1;
				
				c[i] = (double) img.at<uchar>(py[0], px[0])*(px[1]-ix)*(py[2]-iy) + 
					   (double) img.at<uchar>(py[1], px[1])*(ix-px[0])*(py[2]-iy) + 
					   (double) img.at<uchar>(py[2], px[2])*(px[1]-ix)*(iy-py[0]) + 
					   (double) img.at<uchar>(py[3], px[3])*(ix-px[0])*(iy-py[0]);

				mean += c[i];
			} // end of camera

			mean /= camNum;

			for (int i = 0; i < camNum; i++) {
				avgSad += (c[i]-mean)*(c[i]-mean);
			}

			fitness += (*it++) * avgSad;
		} // end of warping y
	} // end of warping x

	delete [] c;
	return fitness / camNum;
}