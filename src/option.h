#ifndef OPTION_H_
#define OPTION_H_

// using namespace std;

class OPTION
{
public:
	int nproc;
	bool filterNoisy;
	double filterStrength;
	double penalty_indel;
	double penalty_mismatch;
        double accuracy;
        int phaseFlank;
        string download;
	OPTION ()
	{
		nproc = 1;
		filterNoisy = false;
		filterStrength = 0.0;
		penalty_indel = 1.0;
		penalty_mismatch = 1.0;
		accuracy=0.98;
		phaseFlank=3000;
                download="";
	};

	~OPTION () {};
};

#endif
