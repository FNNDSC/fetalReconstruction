/*=========================================================================
* GPU accelerated motion compensation for MRI
*
* Copyright (c) 2016 Bernhard Kainz, Amir Alansary, Maria Kuklisova-Murgasova,
* Kevin Keraudren, Markus Steinberger
* (b.kainz@imperial.ac.uk)
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
=========================================================================*/

#include <irtkImage.h>
#include <irtkTransformation.h>
#include <irtkReconstructionGPU.h>
#include <irtkResampling.h>
#include <vector>
#include <string>
#include <perfstats.h>
#include <fstream>
#include <iostream>
#include <time.h>  
//#include <irtkEvaluation.h>
#include <boost/program_options.hpp>
namespace po = boost::program_options;

// include SLIC files
#include <runSLIC_2D.c> 

#if HAVE_CULA
#include "stackMotionEstimator.h"
#endif

using namespace std;

const std::string currentDateTime() {
  time_t     now = time(0);
  struct tm  tstruct;
  char       buf[80];
  tstruct = *localtime(&now);
  strftime(buf, sizeof(buf), "%Y-%m-%d", &tstruct);

  return buf;
}


//TODO
//exclude patches at an early stage for registration
//define patches only virtually and rewrite all kernels so that they only take the patch informatio instead of storing redundat overlapping patches
//apply this in 4D to cardiac images
//learn a dictionary from the patches and learn? registration (for cardiac maybe more advisable)
//integrate SLIC algorithm for patch generation
//integrate niftireg or other GPU based registration algos to be more flexibility


int main(int argc, char **argv)
{
  std::cout << "starting reconstruction on " << currentDateTime() << std::endl;
  //utility variables
  int i, ok;
  char buffer[256];
  irtkRealImage stack;

  //declare variables for input
  /// Slice stacks
  vector<irtkRealImage> stacks;
  /// Stack transformation
  vector<irtkRigidTransformation> stack_transformations;
  /// Stack thickness
  vector<double > thickness;
  ///number of stacks
  int nStacks;
  /// number of packages for each stack
  vector<int> packages;

  vector<float> stackMotion;

  // Default values.
  int templateNumber = -1;
  irtkRealImage *mask = NULL;
  int iterations = 9; //9 //2 for Shepp-Logan is enough
  bool debug = false;
  bool debug_gpu = false;
  double sigma = 20;
  double resolution = 0.75;
  double lambda = 0.02;
  double delta = 150;
  int levels = 3;
  double lastIterLambda = 0.01;
  int rec_iterations;
  double averageValue = 700;
  double smooth_mask = 4;
  bool global_bias_correction = false;
  double low_intensity_cutoff = 0.01;
  //folder for slice-to-volume registrations, if given
  string tfolder;
  //folder to replace slices with registered slices, if given
  string sfolder;
  //flag to swich the intensity matching on and off
  bool intensity_matching = true;
  unsigned int rec_iterations_first = 4;
  unsigned int rec_iterations_last = 13;

  bool useCPU = false;
  bool useCPUReg = true;
  bool useGPUReg = false;
  bool disableBiasCorr = true;
  bool useAutoTemplate = false;

  irtkRealImage average;

  string log_id;
  bool no_log = false;

  //forced exclusion of slices
  int number_of_force_excluded_slices = 0;
  vector<int> force_excluded;
  vector<int> devicesToUse;

  vector<string> inputStacks;
  vector<string> inputTransformations;
  string maskName;
  /// Name for output volume
  string outputName;
  unsigned int num_input_stacks_tuner = 0;
  string referenceVolumeName;
  unsigned int T1PackageSize = 0;
  bool patchBased = false;

  //--------------------------------------------------------------------------------------------
  // superpixel (spx)
  irtkRealImage sStack;
  vector<irtkRealImage> sStacks;
  float noSuperpixels;
  bool superpixelBased = false;
  //--------------------------------------------------------------------------------------------
  
  unsigned int patchSize = 64;
  unsigned int patchStride = 32;
  bool saveSliceTransformations = false;
  bool useNMI = false;

  //in case of manual mask transformation, it is required that the provided manual mask fits the first of the provided image stacks.
  std::string manualMaskName;

  try
  {
    po::options_description desc("Options");
    desc.add_options()
      ("help,h", "Print usage messages")
      ("output,o", po::value<string>(&outputName)->required(), "Name for the reconstructed volume. Nifti or Analyze format.")
      ("mask,m", po::value<string>(&maskName), "Binary mask to define the region od interest. Nifti or Analyze format.")
      ("input,i", po::value<vector<string> >(&inputStacks)->multitoken(), "[stack_1] .. [stack_N]  The input stacks. Nifti or Analyze format.")
      ("transformation,t", po::value< vector<string> >(&inputTransformations)->multitoken(), "The transformations of the input stack to template in \'dof\' format used in IRTK. Only rough alignment with correct orienation and some overlap is needed. Use \'id\' for an identity transformation for at least one stack. The first stack with \'id\' transformation  will be resampled as template.")
      ("thickness", po::value< vector<double> >(&thickness)->multitoken(), "[th_1] .. [th_N] Give slice thickness.[Default: twice voxel size in z direction]")
      ("packages,p", po::value< vector<int> >(&packages)->multitoken(), "Give number of packages used during acquisition for each stack. The stacks will be split into packages during registration iteration 1 and then into odd and even slices within each package during registration iteration 2. The method will then continue with slice to  volume approach. [Default: slice to volume registration only]")
      ("iterations", po::value<int>(&iterations)->default_value(4), "Number of registration-reconstruction iterations.")
      ("sigma", po::value< double >(&sigma)->default_value(12.0), "Stdev for bias field. [Default: 12mm]")
      ("resolution", po::value< double >(&resolution)->default_value(0.75), "Isotropic resolution of the volume. [Default: 0.75mm]")
      ("multires", po::value< int >(&levels)->default_value(3), "Multiresolution smooting with given number of levels. [Default: 3]")
      ("average", po::value< double >(&averageValue)->default_value(700), "Average intensity value for stacks [Default: 700]")
      ("delta", po::value< double >(&delta)->default_value(150), " Parameter to define what is an edge. [Default: 150]")
      ("lambda", po::value< double >(&lambda)->default_value(0.02), "  Smoothing parameter. [Default: 0.02]")
      ("lastIterLambda", po::value< double >(&lastIterLambda)->default_value(0.01), "Smoothing parameter for last iteration. [Default: 0.01]")
      ("smooth_mask", po::value< double >(&smooth_mask)->default_value(4), "Smooth the mask to reduce artefacts of manual segmentation. [Default: 4mm]")
      ("global_bias_correction", po::value< bool >(&global_bias_correction)->default_value(false), "Correct the bias in reconstructed image against previous estimation.")
      ("low_intensity_cutoff", po::value< double >(&low_intensity_cutoff)->default_value(0.01), "Lower intensity threshold for inclusion of voxels in global bias correction.")
      ("force_exclude", po::value< vector<int> >(&force_excluded)->multitoken(), "force_exclude [number of slices] [ind1] ... [indN]  Force exclusion of slices with these indices.")
      ("no_intensity_matching", po::value< bool >(&intensity_matching), "Switch off intensity matching.")
      ("log_prefix", po::value< string >(&log_id), "Prefix for the log file.")
      ("debug", po::value< bool >(&debug)->default_value(false), " Debug mode - save intermediate results.")
      ("debug_gpu", po::bool_switch(&debug_gpu)->default_value(false), " Debug only GPU results.")
      ("rec_iterations_first", po::value< unsigned int >(&rec_iterations_first)->default_value(4), " Set number of superresolution iterations")
      ("rec_iterations_last", po::value< unsigned int >(&rec_iterations_last)->default_value(13), " Set number of superresolution iterations for the last iteration")
      ("num_stacks_tuner", po::value< unsigned int >(&num_input_stacks_tuner)->default_value(0), "  Set number of input stacks that are really used (for tuner evaluation, use only first x)")
      ("no_log", po::value< bool >(&no_log)->default_value(false), "  Do not redirect cout and cerr to log files.")
      ("devices,d", po::value< vector<int> >(&devicesToUse)->multitoken(), "  Select the CP > 3.0 GPUs on which the reconstruction should be executed. Default: all devices > CP 3.0")
      ("tfolder", po::value< string >(&tfolder), "[folder] Use existing slice-to-volume transformations to initialize the reconstruction.")
      ("sfolder", po::value< string >(&sfolder), "[folder] Use existing registered slices and replace loaded ones (have to be equally many as loaded from stacks).")
      ("referenceVolume", po::value<string>(&referenceVolumeName), "Name for an optional reference volume. Will be used as inital reconstruction.")
      ("T1PackageSize", po::value<unsigned int>(&T1PackageSize), "is a test if you can register T1 to T2 using NMI and only one iteration")
      ("useCPU", po::bool_switch(&useCPU)->default_value(false), "use CPU for reconstruction and registration; performs superresolution and robust statistics on CPU. Default is using the GPU")
      ("useCPUReg", po::bool_switch(&useCPUReg)->default_value(true), "use CPU for more flexible CPU registration; performs superresolution and robust statistics on GPU. [default, best result]")
      ("useGPUReg", po::bool_switch(&useGPUReg)->default_value(false), "use faster but less accurate and flexible GPU registration; performs superresolution and robust statistics on GPU.")
      ("useAutoTemplate", po::bool_switch(&useAutoTemplate)->default_value(false), "select 3D registration template stack automatically with matrix rank method.")
      ("patchSize", po::value< unsigned int >(&patchSize)->default_value(64), "defines the size of the 2D patches for patchBased reconstruction")
      ("patchStride", po::value< unsigned int >(&patchStride)->default_value(32), "defines the stride of the 2D patches for patchBased reconstruction")
      ("disableBiasCorrection", po::bool_switch(&disableBiasCorr)->default_value(true), "disable bias field correction for cases with little or no bias field inhomogenities (makes it faster but less reliable for stron intensity bias)")
      ("patchBased", po::bool_switch(&patchBased)->default_value(false), "activate experimentl patch-based reconstruction.")
      //--------------------------------------------------------------------------------------------
      // superpixel (spx)
      ("superpixelBased", po::bool_switch(&superpixelBased)->default_value(false), "activate experimentl superpixel-based reconstruction.")
      ("superpixel,s", po::value< float >(&noSuperpixels)->multitoken(), "[noLabels] Number of superpixels")
      //--------------------------------------------------------------------------------------------
      ("manualMask", po::value<string>(&manualMaskName), "Binary manual accurate mask to define a region accuratly slice by slice. It is required that the provided manual mask fits the *first* of the provided image stacks in -i <stacks *1*...N>! Nifti or Analyze format.")
      ("useNMI", po::bool_switch(&useNMI)->default_value(false), "use Normalized Mutual Information for slice to volume registration.")
      ("saveSliceTransformations", po::bool_switch(&saveSliceTransformations)->default_value(false), "Save slice transformations and pixel to voxel mapping. Be aware that the index refers to the stacks cropped with the provided mask (not the original stack slice index).");
    po::variables_map vm;

    try
    {
      po::store(po::parse_command_line(argc, argv, desc), vm); // can throw 

      if (vm.count("help"))
      {
        std::cout << "Application to perform reconstruction of volumetric MRI from thick slices." << std::endl
          << desc << std::endl;
        return EXIT_SUCCESS;
      }

      po::notify(vm);
    }
    catch (po::error& e)
    {
      std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
      std::cerr << desc << std::endl;
      return EXIT_FAILURE;
    }
  }
  catch (std::exception& e)
  {
    std::cerr << "Unhandled exception while parsing arguments:  "
      << e.what() << ", application will now exit" << std::endl;
    return EXIT_FAILURE;
  }

  if (useCPU)
  {
    useCPUReg = true;
    useGPUReg = false;
  }

  if (useGPUReg) useCPUReg = false;

  cout << "Reconstructed volume name ... " << outputName << endl;
  nStacks = inputStacks.size();
  cout << "Number of stacks ... " << nStacks << endl;

  irtkRealImage referenceVolume;
  if (!referenceVolumeName.empty())
  {
    referenceVolume.Read(referenceVolumeName.c_str());
    cout << "using " << referenceVolumeName << " as initial reference volume for " << outputName << endl;
  }

  bool needManualMaskTransform = false;
  irtkRealImage manualMask;
  if(!manualMaskName.empty())
  {
    manualMask.Read(manualMaskName.c_str());
    needManualMaskTransform = true;
  }
  
  vector<double> thicknessN;
  vector<int> packagesN;  

  float tmp_motionestimate = FLT_MAX;
  for (i = 0; i < nStacks; i++)
  {
    stack.Read(inputStacks[i].c_str());
    cout << "Reading stack ... " << inputStacks[i] << endl;
    //stacks.push_back(stack);

	//separate 4D volumes
	if (stack.GetT() > 1)
	{
		irtkImageAttributes attr = stack.GetImageAttributes();
		attr._t = 1;
		for (int t = 0; t < stack.GetT(); t++)
		{
			cout << "Splitting stack ... " << inputStacks[i] << endl;
			irtkRealImage stackn(attr);
			memcpy(stackn.GetPointerToVoxels(), stack.GetPointerToVoxels() + t*stack.GetX()*stack.GetY()*stack.GetZ(),
				stack.GetX()*stack.GetY()*stack.GetZ() * sizeof(double));
			stacks.push_back(stackn);
			if (!thickness.empty())
				thicknessN.push_back(thickness[i]);
			if (!packages.empty())
				packagesN.push_back(packages[i]);
		}
	}
	else
	{
		stacks.push_back(stack);
		if (!thickness.empty())
			thicknessN.push_back(thickness[i]);
		if (!packages.empty())
			packagesN.push_back(packages[i]);
	}

	if (!thickness.empty())
		thickness = thicknessN;
	if (!packages.empty())
		packages = packagesN;
    //--------------------------------------------------------------------------------------------
    // superpixel (spx)
    if (superpixelBased)
    {
      float noLabels = noSuperpixels;
      double c = 1; // compactness factor -- control the superpixel shape
      runSLIC_2D(noLabels, c, stack, sStacks);
    }
    //--------------------------------------------------------------------------------------------
  }

  for (int i = 0; i < stacks.size(); i++)
  {
	  //irtkReconstruction::Rescale(stacks[i], 4096.0);
	  sprintf(buffer, "stack%i.nii", i);
	  stacks[i].Write(buffer);
  }

  nStacks = stacks.size();

  for (i = 0; i < nStacks; i++)
  {
    irtkTransformation *transformation;
    if (!inputTransformations.empty())
    {
      if(inputTransformations[i] == std::string("id"))
      {
        if (templateNumber < 0) templateNumber = i;
        transformation = new irtkRigidTransformation;
      }
      else
      {
        transformation = irtkTransformation::New((char*)(inputTransformations[i].c_str()));
      }
    }
    else
    {
      transformation = new irtkRigidTransformation;
      if (templateNumber < 0) templateNumber = 0;
    }

    irtkRigidTransformation *rigidTransf = dynamic_cast<irtkRigidTransformation*> (transformation);
    stack_transformations.push_back(*rigidTransf);
    delete rigidTransf;
  }

  if (!useCPU)
  {
    cudaDeviceReset();
    //default use all devices > CP 3.0 that are available
    int nDevices;
    cudaGetDeviceCount(&nDevices);
    if (nDevices < devicesToUse.size())
    {
      std::cerr << "FATAL ERROR: you cannot use more GPUs than you have in your rig. defaulting to max CP > 3.0 devices. " << std::endl;
      devicesToUse.clear();
    }
    if (devicesToUse.empty())
    {
      for (int i = 0; i < nDevices; i++) {
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, i);
        if (prop.major >= 3)
        {
          devicesToUse.push_back(i);
        }
      }
    }
    if (devicesToUse.empty())
    {
      std::cerr << "FATAL ERROR: no suitable devices with compute capability > 3.0 found that yre available. exiting" << std::endl;
      return EXIT_FAILURE;
    }
  }

  //Create reconstruction object
  // !useCPUReg = no multithreaded GPU, only multi-GPU
  irtkReconstruction reconstruction(devicesToUse, useCPUReg); // to emulate error for multi-threaded GPU

  reconstruction.setPatchBased(patchBased, useCPU);
  if (disableBiasCorr){
    reconstruction.disableBiasCorrection();
  }

  //--------------------------------------------------------------------------------------------
  // superpixel (spx)
  reconstruction.setSuperpixelBased(superpixelBased, useCPU);
  //--------------------------------------------------------------------------------------------
  reconstruction.Set_debugGPU(debug_gpu);

  reconstruction.InvertStackTransformations(stack_transformations);

  if (!maskName.empty())
  {
    mask = new irtkRealImage((char*)(maskName.c_str()));
  }

  if (num_input_stacks_tuner > 0)
  {
    nStacks = num_input_stacks_tuner;
    cout << "actually used stacks for tuner test .... " << num_input_stacks_tuner << endl;
  }

  number_of_force_excluded_slices = force_excluded.size();

  //erase stacks for tuner evaluation
  if (num_input_stacks_tuner > 0)
  {
    stacks.erase(stacks.begin() + num_input_stacks_tuner, stacks.end());
    stack_transformations.erase(stack_transformations.begin() + num_input_stacks_tuner, stack_transformations.end());
    std::cout << "stack sizes: " << nStacks << " " << stacks.size() << " " << thickness.size() << " " << stack_transformations.size() << std::endl;
  }

  //Initialise 2*slice thickness if not given by user
  if (thickness.size() == 0)
  {
    cout << "Slice thickness is ";
    for (i = 0; i < nStacks; i++)
    {
      double dx, dy, dz;
      stacks[i].GetPixelSize(&dx, &dy, &dz);
      thickness.push_back(dz * 2);
      cout << thickness[i] << " ";
    }
    cout << "." << endl;
  }

  //Output volume
  irtkRealImage reconstructed;
  irtkRealImage lastReconstructed;
  irtkRealImage reconstructedGPU;

  std::vector<double> samplingUcert;

  //Set debug mode
  if (debug) reconstruction.DebugOn();
  else reconstruction.DebugOff();

  //Set force excluded slices
  reconstruction.SetForceExcludedSlices(force_excluded);

  //Set low intensity cutoff for bias estimation
  reconstruction.SetLowIntensityCutoff(low_intensity_cutoff);


  // Check whether the template stack can be indentified
  if (templateNumber < 0)
  {
    cerr << "Please identify the template by assigning id transformation." << endl;
    exit(1);
  }
  //If no mask was given  try to create mask from the template image in case it was padded
  if ((mask == NULL) && (sfolder.empty()))
  {
    //TODO calculate overlap area. Make mask from that
    mask = new irtkRealImage(stacks[templateNumber]);
    if (patchBased)
    {
      *mask = reconstruction.CreateMaskFromOverlap(stacks);
    }
    //--------------------------------------------------------------------------------------------
    // superpixel (spx)
    else if (superpixelBased)
    {
      *mask = reconstruction.CreateMaskFromOverlap(stacks);
    }
    //--------------------------------------------------------------------------------------------
    else
    {
      *mask = reconstruction.CreateMask(*mask);
    }

    mask->Write("generatedMask.nii.gz");
  }

  //copy to tmp stacks for template determination
  std::vector<irtkRealImage> tmpStacks;
  for (i = 0; i < stacks.size(); i++)
  {
    tmpStacks.push_back(stacks[i]);
  }

  PerfStats stats;
  stats.start();

  if (T1PackageSize > 0)
  {
    cout << "using " << referenceVolumeName << " as T2 reference reconstruction and going to register T1 packages to it." << endl;
    cout << "T1 Package Size is " << T1PackageSize << std::endl;

    std::vector<irtkRealImage> stackPackages;
    for (int i = 0; i < stacks.size(); i++)
    {
      reconstruction.SplitImage(stacks[i], T1PackageSize, stackPackages);
    }


    std::cout << "got " << stackPackages.size() << " package-stacks" << std::endl;

    std::vector<irtkRigidTransformation> stackPackages_transformations;

    for (i = 0; i < stackPackages.size(); i++)
    {
      irtkTransformation *transformation;
      transformation = new irtkRigidTransformation;

      irtkRigidTransformation *rigidTransf = dynamic_cast<irtkRigidTransformation*> (transformation);
      stackPackages_transformations.push_back(*rigidTransf);
      delete rigidTransf;
    }

    reconstruction.externalRegistrationTargetImage = referenceVolume;
    reconstruction.InvertStackTransformations(stackPackages_transformations);

    if (mask != NULL)
    {
      irtkRealImage m = *mask;
      reconstruction.TransformMask(stackPackages[templateNumber], m, stackPackages_transformations[templateNumber]);
      reconstruction.CropImage(stackPackages[templateNumber], m);
    }

    if (debug)
      stackPackages[20].Write("testPackage.nii.gz");

    //package to reconstruction registration
    //reconstruction.StackRegistrations(stacks,stack_transformations,templateNumber);
    //fill stack_transformations
    //reconstruction.StackRegistrations(stackPackages,stackPackages_transformations,0,true);

    //hack
    stacks = stackPackages;
    stack_transformations = stackPackages_transformations;
    nStacks = stackPackages.size();

    vector<double > package_slicethickness;

    cout << "Slice thickness is ";
    for (i = 0; i < nStacks; i++)
    {
      double dx, dy, dz;
      stackPackages[i].GetPixelSize(&dx, &dy, &dz);
      package_slicethickness.push_back(2.0 * 2);  //(dz*2);
      cout << package_slicethickness[i] << " ";
    }
    cout << "." << endl;

    thickness = package_slicethickness;
  }

  //Before creating the template we will crop template stack according to the given mask
  if (mask != NULL)
  {
    //first resample the mask to the space of the stack
    //for template stact the transformation is identity
    irtkRealImage m = *mask;

#if HAVE_CULA
    if(useAutoTemplate)
    {
      //TODO would be better to do this for the masked stacks...
      {
        stackMotionEstimator mestimate;
        for(i = 0; i < stacks.size(); i++)
        {
          reconstruction.TransformMask(tmpStacks[i],m,stack_transformations[i]);
          //Crop template stack
          reconstruction.CropImage(tmpStacks[i],m);
          //now define template with motion estimation...

          float motionestimate = mestimate.evaluateStackMotion(&tmpStacks[i]);
          std::cout << "estimated motion: " << motionestimate << std::endl;
          stackMotion.push_back(motionestimate);
          if(motionestimate < tmp_motionestimate)
          {
            templateNumber = i;
            tmp_motionestimate = motionestimate;
          }
        }
      }
      stats.sample("motion measurement");
      std::cout << "Determined stack " << templateNumber << " as template. " << std::endl;
  }
#endif

  //  return EXIT_SUCCESS;

    //now do it really with best stack
    reconstruction.TransformMask(stacks[templateNumber], m, stack_transformations[templateNumber]);
    //Crop template stack
    reconstruction.CropImage(stacks[templateNumber], m);
    if (superpixelBased)
    {
      reconstruction.CropImage(sStacks[templateNumber], m);
    }

    if (debug)
    {
      m.Write("maskTemplate.nii.gz");
      stacks[templateNumber].Write("croppedTemplate.nii.gz");
    }
}

  //mask->Write("generatedMask_t.nii.gz");
  tmpStacks.erase(tmpStacks.begin(), tmpStacks.end());

  std::vector<uint3> stack_sizes;
  //std::cout << "Stack sizes: "<< std::endl;
  for (int i = 0; i < stacks.size(); i++)
  {
    stack_sizes.push_back(make_uint3(stacks[i].GetX(), stacks[i].GetY(), stacks[i].GetZ()));
    //	std::cout << stack_sizes[i].x << " " << stack_sizes[i].y << " " << stack_sizes[i].z << " " << std::endl;
  }

  //Create template volume with isotropic resolution 
  //if resolution==0 it will be determined from in-plane resolution of the image
  resolution = reconstruction.CreateTemplate(stacks[templateNumber], resolution);

  //Set mask to reconstruction object. 
  reconstruction.SetMask(mask, smooth_mask);

  //reconstruction.GetMask().Write("generatedMask_s.nii");
  //to redirect output from screen to text files

  //to remember cout and cerr buffer
  streambuf* strm_buffer = cout.rdbuf();
  streambuf* strm_buffer_e = cerr.rdbuf();
  //files for registration output
  string name;
  name = log_id + "log-registration.txt";
  ofstream file(name.c_str());
  name = log_id + "log-registration-error.txt";
  ofstream file_e(name.c_str());
  //files for reconstruction output
  name = log_id + "log-reconstruction.txt";
  ofstream file2(name.c_str());
  name = log_id + "log-evaluation.txt";
  ofstream fileEv(name.c_str());

  //set precision
  cout << setprecision(3);
  cerr << setprecision(3);

  //perform volumetric registration of the stacks
  //redirect output to files
  if (!no_log) {
    cerr.rdbuf(file_e.rdbuf());
    cout.rdbuf(file.rdbuf());
  }

  if (T1PackageSize == 0 && sfolder.empty())
  {
    //volumetric registration
    reconstruction.StackRegistrations(stacks, stack_transformations, templateNumber);
  }

  //return EXIT_SUCCESS;

  cout << endl;
  //redirect output back to screen
  if (!no_log) {
    cout.rdbuf(strm_buffer);
    cerr.rdbuf(strm_buffer_e);
  }


  average = reconstruction.CreateAverage(stacks, stack_transformations);
  if (debug)
    average.Write("average1.nii.gz");

  //Mask is transformed to the all other stacks and they are cropped
  for (i = 0; i < nStacks; i++)
  {
    //template stack has been cropped already
    if ((i == templateNumber)) continue;
    //transform the mask
    irtkRealImage m = reconstruction.GetMask();
    reconstruction.TransformMask(stacks[i], m, stack_transformations[i]);
    //Crop template stack
    reconstruction.CropImage(stacks[i], m);
    if (!sStacks.empty())
    {
      reconstruction.CropImage(sStacks[i], m);
    }
    if (debug)
    {
      sprintf(buffer, "mask%i.nii.gz", i);
      m.Write(buffer);
      sprintf(buffer, "cropped%i.nii.gz", i);
      stacks[i].Write(buffer);
    }
  }

  //Repeat volumetric registrations with cropped stacks
  //redirect output to files
  if (!no_log) {
    cerr.rdbuf(file_e.rdbuf());
    cout.rdbuf(file.rdbuf());
  }

  if (T1PackageSize == 0 && sfolder.empty())
  {
    //volumetric registration
    reconstruction.StackRegistrations(stacks, stack_transformations, templateNumber);
    cout << endl;
  }

  //redirect output back to screen
  if (!no_log) {
    cout.rdbuf(strm_buffer);
    cerr.rdbuf(strm_buffer_e);
  }

  //Rescale intensities of the stacks to have the same average
  if (intensity_matching)
    reconstruction.MatchStackIntensitiesWithMasking(stacks, stack_transformations, averageValue);
  else
    reconstruction.MatchStackIntensitiesWithMasking(stacks, stack_transformations, averageValue, true);
  average = reconstruction.CreateAverage(stacks, stack_transformations);
  if (debug)
    average.Write("average2.nii.gz");
  //exit(1);

  //Create slices and slice-dependent transformations
  //resolution = reconstruction.CreateTemplate(stacks[templateNumber],resolution);
  if(patchBased)
  {
    reconstruction.CreateSlicesAndTransformationsPatchBased(patchSize, patchStride, stacks, stack_transformations, thickness);
  }
  //--------------------------------------------------------------------------------------------
  // superpixel (spx)
  else if (superpixelBased)
  {
    reconstruction.CreateSlicesAndTransformationsSuperpixelBased(sStacks, stacks, stack_transformations, thickness);
  }
  //--------------------------------------------------------------------------------------------
  else
  {
    reconstruction.CreateSlicesAndTransformations(stacks, stack_transformations, thickness);
  }

  if (!sfolder.empty())
  {
    //TODO replace slices for US experiment
    reconstruction.replaceSlices(sfolder);
  }

  //Mask all the slices
  reconstruction.MaskSlices();

  /* if(!tfolder.empty())
   {
   reconstruction.ReadTransformation(folder);
   }*/


  //Set sigma for the bias field smoothing
  if (sigma > 0)
    reconstruction.SetSigma(sigma);
  else
  {
    //cerr<<"Please set sigma larger than zero. Current value: "<<sigma<<endl;
    //exit(1);
    reconstruction.SetSigma(20);
  }

  //Set global bias correction flag
  if (global_bias_correction)
    reconstruction.GlobalBiasCorrectionOn();
  else
    reconstruction.GlobalBiasCorrectionOff();

  //if given read slice-to-volume registrations
  if (!tfolder.empty())
    reconstruction.ReadTransformation((char*)tfolder.c_str());

  stats.sample("overhead/setup");
  pt::ptime tick = pt::microsec_clock::local_time();


  if (!useCPU)
  {
    //get data on GPU
    reconstruction.SyncGPU();
    if (!useCPUReg)
    {
      reconstruction.PrepareRegistrationSlices();
    }
    //return EXIT_SUCCESS;
    stats.sample("SyncGPU");
  }

  //Initialise data structures for EM
  if (useCPU)
  {
    reconstruction.InitializeEM();
  }
  else {
    reconstruction.InitializeEMGPU();
  }
  stats.sample("InitializeEM");

  if (!useCPU)
  {
    //only one update
    reconstruction.UpdateGPUTranformationMatrices();
  }

  //interleaved registration-reconstruction iterations
  for (int iter = 0; iter < iterations; iter++)
  {
    //Print iteration number on the screen
    if (!no_log) {
      cout.rdbuf(strm_buffer);
    }
    cout << "Iteration " << iter << ". " << endl;

    //perform slice-to-volume registrations - skip the first iteration 
	if (iter > 0 || !referenceVolumeName.empty())
    {
      if (!no_log) {
        cerr.rdbuf(file_e.rdbuf());
        cout.rdbuf(file.rdbuf());
      }
      cout << "Iteration " << iter << ": " << endl;
      cout << "Slice To Volume Registration " << ": " << endl;
      //if((packages.size()>0)&&(iter<(iterations-1)))
      if ((packages.size() > 0) && (iter <= iterations*(levels - 1) / levels) && (iter < (iterations - 1)))
      {
        if (iter == 1)
          reconstruction.PackageToVolume(stacks, packages);
        else
        {
          if (iter == 2)
            reconstruction.PackageToVolume(stacks, packages, true);
          else
          {
            if (iter == 3)
              reconstruction.PackageToVolume(stacks, packages, true, true);
            else
            {
              if (iter >= 4)
                reconstruction.PackageToVolume(stacks, packages, true, true, iter - 2);
              else
                printf("unexpected program path");
              if (useCPUReg)
              {
                cout << "Slice To Volume Registration CPU" << ": " << endl;
                reconstruction.SliceToVolumeRegistration();
              }
              else {
                cout << "Slice To Volume Registration GPU" << ": " << endl;
                reconstruction.SliceToVolumeRegistrationGPU();
              }
              stats.sample("Registration");
            }
          }
        }
      }
      else
        if (useCPUReg)
        {
        printf("Slice To Volume Registration CPU\n");
        cout << "Slice To Volume Registration CPU" << ": " << endl;
        reconstruction.SliceToVolumeRegistration();
        //reconstruction.testCPURegGPU();
        }
        else {
          printf("Slice To Volume Registration GPU\n");
          cout << "Slice To Volume Registration GPU" << ": " << endl;
          reconstruction.SliceToVolumeRegistrationGPU();
        }
        stats.sample("Registration");

        cout << endl;
        if (!no_log) {
          cerr.rdbuf(strm_buffer_e);
        }
    }

    //Write to file
    if (!no_log) {
      cout.rdbuf(file2.rdbuf());
    }
    cout << endl << endl << "Iteration " << iter << ": " << endl << endl;

    //Set smoothing parameters 
    //amount of smoothing (given by lambda) is decreased with improving alignment
    //delta (to determine edges) stays constant throughout

    //this causes a drift for very homogenous (phantom) data
    //reconstruction.SetSmoothingParameters(delta,lastIterLambda);
    if (iter == (iterations - 1))
      reconstruction.SetSmoothingParameters(delta, lastIterLambda);
    else
    {
      double l = lambda;
      for (i = 0; i < levels; i++)
      {
        if (iter == iterations*(levels - i - 1) / levels)
          reconstruction.SetSmoothingParameters(delta, l);
        l *= 2;
      }
    }

    //Use faster reconstruction during iterations and slower for final reconstruction
    if (iter < (iterations - 1))
    {
      reconstruction.SpeedupOn();
    }
    else
    {
      reconstruction.SpeedupOff();
    }
    if (!useCPU)
    {
      reconstruction.generatePSFVolume();
      stats.sample("generatePSFVolume");
    }


    //Initialise values of weights, scales and bias fields
    if (useCPU)
    {
      reconstruction.InitializeEMValues();
    }
    else {
      reconstruction.InitializeEMValuesGPU();
    }
    stats.sample("InitializeEMValues");

    //Calculate matrix of transformation between voxels of slices and volume
    if (useCPU)
    {
      reconstruction.CoeffInit();
    }
    else {
      reconstruction.UpdateGPUTranformationMatrices();
    }
    stats.sample("CoeffInit");

    //Initialize reconstructed image with Gaussian weighted reconstruction
    if (useCPU)
    {
      reconstruction.GaussianReconstruction();
      if (debug)
      {
        reconstructed = reconstruction.GetReconstructed();
        sprintf(buffer, "GaussianReconstruction_CPU%i.nii", iter);
        reconstructed.Write(buffer);
      }
    }
    else {
      reconstruction.GaussianReconstructionGPU();
      if (true /*debug || debug_gpu*/)
      {
        reconstructedGPU = reconstruction.GetReconstructedGPU();
        sprintf(buffer, "GaussianReconstruction_GPU%i.nii", iter);
        reconstructedGPU.Write(buffer);
      }
    }
    stats.sample("GaussianReconstruction");

   // return EXIT_SUCCESS;
    //Simulate slices (needs to be done after Gaussian reconstruction)
    if (useCPU)
    {
      reconstruction.SimulateSlices();
    }
    else {
      reconstruction.SimulateSlicesGPU();
    }
    stats.sample("SimulateSlices");

    //Initialize robust statistics parameters
    if (useCPU)
    {
      reconstruction.InitializeRobustStatistics();
    }
    else {
      reconstruction.InitializeRobustStatisticsGPU();
    }
    stats.sample("InitializeRS");

    //EStep
    if (useCPU)
    {
      reconstruction.EStep();
    }
    else {
      reconstruction.EStepGPU();
    }
    stats.sample("EStep");
    //return EXIT_SUCCESS; 

    //number of reconstruction iterations
    if (iter == (iterations - 1))
    {
      rec_iterations = rec_iterations_last;
    }
    else
      rec_iterations = rec_iterations_first;

    //reconstruction iterations
    i = 0;
    for (i = 0; i < rec_iterations; i++)
    {

      cout << endl << "  Reconstruction iteration " << i << ". " << endl;

      if (intensity_matching)
      {
        //calculate bias fields
        if (useCPU)
        {
          if (!disableBiasCorr)
          {
            if (sigma > 0)
              reconstruction.Bias();
          }
          //calculate scales
          reconstruction.Scale();
        }
        else {
          //TODO try out N4 bias correction
          if (!disableBiasCorr)
          {
            if (sigma > 0)
              reconstruction.BiasGPU();
          }
          //calculate scales
          reconstruction.ScaleGPU();
        }
        stats.sample("Bias and Scale");
      }

      //MStep and update reconstructed volume
      if (useCPU)
      {
        reconstruction.Superresolution(i + 1);

#if 0
        reconstructed = reconstruction.GetReconstructed();
        sprintf(buffer, "superCPU%i.nii", i);
        reconstructed.Write(buffer);
#endif
      }
      else {
        reconstruction.SuperresolutionGPU(i + 1);
      }
      stats.sample("Superresolution");

      //return EXIT_SUCCESS; 

      if (intensity_matching)
      {
        if (!disableBiasCorr)
        {

          if (useCPU)
          {
            if ((sigma > 0) && (!global_bias_correction))
              reconstruction.NormaliseBias(i);
          }
          else {
            if ((sigma > 0) && (!global_bias_correction))
              reconstruction.NormaliseBiasGPU(i);
          }

        }
        stats.sample("NormaliseBias");
      }

      // Simulate slices (needs to be done
      // after the update of the reconstructed volume)
      if (useCPU)
      {
        reconstruction.SimulateSlices();
      }
      else {
	      //printf("2nd simulate slices");
        reconstruction.SimulateSlicesGPU();
      }
      stats.sample("SimulateSlices");
      if (useCPU)
      {
        reconstruction.MStep(i + 1);
      }
      else {
        reconstruction.MStepGPU(i + 1);
      }
      stats.sample("MStep");
      if (useCPU)
      {
        //E-step
        reconstruction.EStep();
      }
      else {
        reconstruction.EStepGPU();
      }
      stats.sample("EStep");

      //Save intermediate reconstructed image
      if (debug || debug_gpu)
      {
        if (useCPU)
        {
          reconstructed = reconstruction.GetReconstructed();
          sprintf(buffer, "superCPU%i.nii", i);
          reconstructed.Write(buffer);
        }
        else {
          reconstructedGPU = reconstruction.GetReconstructedGPU();
          sprintf(buffer, "superGPU%i.nii", i);
          reconstructedGPU.Write(buffer);
        }
      }
      printf("%d ", i);
    }//end of reconstruction iterations

    printf("Main loop end\n");

    //Mask reconstructed image to ROI given by the mask
    if (useCPU)
    {
      reconstruction.MaskVolume();
    }
    else {
      reconstruction.MaskVolumeGPU();
    }
    stats.sample("MaskVolume");

    printf("Masking done\n");

    if (patchBased)
    {
      if (!useCPU)
      {
        printf("writing volWeights\n");
        irtkGenericImage<float> volweights = reconstruction.getVolWeights();
        sprintf(buffer, "volWeights%i_GPU.nii", iter);
        volweights.Write(buffer);
        printf("writing weights\n");
        irtkGenericImage<float> weights = reconstruction.getWeights();
        sprintf(buffer, "weights%i_GPU.nii", iter);
        weights.Write(buffer);
      }
      else
      {
        reconstruction.SaveWeights();
      }
    }
    //--------------------------------------------------------------------------------------------
    // superpixel (spx)
    else if (superpixelBased)
    {
      if (!useCPU)
      {
        printf("writing volWeights\n");
        irtkGenericImage<float> volweights = reconstruction.getVolWeights();
        sprintf(buffer, "volWeights%i_GPU.nii", iter);
        volweights.Write(buffer);
        printf("writing weights\n");
        irtkGenericImage<float> weights = reconstruction.getWeights();
        sprintf(buffer, "weights%i_GPU.nii", iter);
        weights.Write(buffer);
      }
      else
      {
        reconstruction.SaveWeights();
      }
    }

    //Save reconstructed image
    if (useCPU)
    {
      reconstructed = reconstruction.GetReconstructed();
      sprintf(buffer, "image%i_CPU.nii.gz", iter);
      reconstructed.Write(buffer);
    }
    else {
      reconstruction.SyncCPU();
      stats.sample("SyncCPU");
      reconstructed = reconstruction.GetReconstructed();
      sprintf(buffer, "image%i_GPU.nii.gz", iter);
      reconstructed.Write(buffer);
      //get quality gradient
      //TODO implement patch-based quality metric
      /*if (iter > 0)
      {
      irtkEvaluation eval(reconstructed, lastReconstructed);
      EvalResult res = eval.evaluate();
      std::cout << "PSNR: " << res.psnr << std::endl;
      ofstream timefile;
      timefile.open("psnr.txt", ios::out | ios::app);
      timefile << res.psnr << "\n";
      timefile.close();
      }
      else
      {
      //compare difference to first recon
      lastReconstructed = reconstructed;
      }*/
    }

    if (saveSliceTransformations)
    {
      reconstruction.SaveSlices();
      reconstruction.SaveTransformations();
    }

    //Evaluate - write number of included/excluded/outside/zero slices in each iteration in the file
    if (!no_log) {
      cout.rdbuf(fileEv.rdbuf());
    }

    if (useCPU)
    {
      reconstruction.Evaluate(iter);
      cout << endl;
    }
    else {
      reconstruction.EvaluateGPU(iter);
      cout << endl;
    }
    if (!no_log) {
      cout.rdbuf(strm_buffer);
    }
    printf("\n");
  }// end of interleaved registration-reconstruction iterations

  //--------------------------------------------------------------------------------------------
  //addition for manualMask transform (Tong's lung atlas)
  if(needManualMaskTransform)
  {
    //need to crop the mask to same size as first stack -- like fill slices...
    reconstruction.CropImage(manualMask,*mask);
    irtkGenericImage<float>* transformedManualMask = new irtkGenericImage<float>;
    transformedManualMask->Initialize(reconstruction.GetReconstructed().GetImageAttributes());
    reconstruction.transformManualMaskwithPSF(manualMask, transformedManualMask);
    manualMaskName.insert(manualMaskName.find_last_of("/")+1,"PSFTransformed_");
    transformedManualMask->Write(manualMaskName.c_str());
  }

  //reconstruction.SyncCPU();
  if (useCPU)
  {
    reconstruction.RestoreSliceIntensities();
  }
  else {
    reconstruction.RestoreSliceIntensitiesGPU();
  }
  stats.sample("RestoreSliceInt.");

  if (useCPU)
  {
    reconstruction.ScaleVolume();
  }
  else {
    reconstruction.ScaleVolumeGPU();
  }
  stats.sample("ScaleVolume");

  if (!useCPU)
  {
    //final sync
    reconstruction.SyncCPU();
    stats.sample("SyncCPU");
  }

  pt::ptime now = pt::microsec_clock::local_time();
  pt::time_duration diff = now - tick;
  double mss = diff.total_milliseconds() / 1000.0;

  if (useCPU)
  {
    sprintf(buffer, "performance_CPU_%s.txt", currentDateTime().c_str());
  }
  else {
    sprintf(buffer, "performance_GPU_%s.txt", currentDateTime().c_str());
  }
  ofstream perf_file(buffer);
  stats.print();
  stats.print(perf_file);
  perf_file << "\n.........overall time: ";
  perf_file << mss;
  perf_file << " s........\n";
  perf_file.close();
  printf(".........overall time: %f s........\n", mss);

  //save final result
  reconstructed = reconstruction.GetReconstructed();
  reconstructed.Write(outputName.c_str());

  //write computation time to file for tuner test

  /*ofstream timefile;
  timefile.open ("time.txt", ios::out | ios::app);
  timefile << mss << "\n";
  timefile.close();*/
  //The end of main()
}
