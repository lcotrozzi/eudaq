#ifdef WIN32
#include <Windows4Root.h>
#endif

// ROOT includes
#include "TROOT.h"
#include "TNamed.h"
#include "TApplication.h"
#include "TGClient.h"
#include "TGMenu.h"
#include "TGTab.h"
#include "TGButton.h"
#include "TGComboBox.h"
#include "TGLabel.h"
#include "TGTextEntry.h"
#include "TGNumberEntry.h"
#include "TGComboBox.h"
#include "TStyle.h"
#include "TCanvas.h"
#include "TRootEmbeddedCanvas.h"
#include "TH1D.h"
#include "TH2D.h"
#include "TPaletteAxis.h"
#include "TThread.h"
#include "TFile.h"
#include "TColor.h"
#include "TString.h"
#include "TF1.h"
//#include "TSystem.h" // for TProcessEventTimer
// C++ INCLUDES
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <map>
#include <cstring>
#include <stdio.h>
#include <string.h>
#include <chrono>
#include <thread>
#include <memory>

//ONLINE MONITOR Includes
#include "OnlineMon.hh"
#include "eudaq/StandardEvent.hh"
#include "eudaq/StdEventConverter.hh"
using namespace std;

RootMonitor::RootMonitor(const std::string & runcontrol,
			 int /*x*/, int /*y*/, int /*w*/, int /*h*/,
			 int argc, int offline, const std::string & conffile, const std::string & monname)
  :eudaq::Monitor(monname, runcontrol), _offline(offline), _planesInitialized(false), onlinemon(NULL){
  if (_offline <= 0)
  {
    onlinemon = new OnlineMonWindow(gClient->GetRoot(),800,600);
    if (onlinemon==NULL)
    {
      cerr<< "Error Allocationg OnlineMonWindow"<<endl;
      exit(-1);
    }
  }

  m_plane_c = 0;
  m_ev_rec_n = 0;
  
  hmCollection = new HitmapCollection();
  corrCollection = new CorrelationCollection();
  MonitorPerformanceCollection *monCollection =new MonitorPerformanceCollection();
  eudaqCollection = new EUDAQMonitorCollection();
  paraCollection = new ParaMonitorCollection();

  
  cout << "--- Done ---"<<endl<<endl;

  // put collections into the vector
  _colls.push_back(hmCollection);
  _colls.push_back(corrCollection);
  _colls.push_back(monCollection);
  _colls.push_back(eudaqCollection);
  _colls.push_back(paraCollection);

  // set the root Monitor
  if (_offline <= 0) {
    hmCollection->setRootMonitor(this);
    corrCollection->setRootMonitor(this);
    monCollection->setRootMonitor(this);
    eudaqCollection->setRootMonitor(this);
    paraCollection->setRootMonitor(this);

    onlinemon->setCollections(_colls);
  }

  //initialize with default configuration
  mon_configdata.SetDefaults();
  configfilename.assign(conffile);

  if (configfilename.length()>1)
  {
    mon_configdata.setConfigurationFileName(configfilename);
    if (mon_configdata.ReadConfigurationFile()!=0)
    {
      // reset defaults, as Config file is bad
      cerr <<" As Config file can't be found, re-applying hardcoded defaults"<<endl;
      mon_configdata.SetDefaults();

    }
  }


  // print the configuration
  mon_configdata.PrintConfiguration();

  cout << "End of Constructor" << endl;

  //set a few defaults
  snapshotdir=mon_configdata.getSnapShotDir();
  previous_event_analysis_time=0;
  previous_event_fill_time=0;
  previous_event_clustering_time=0;
  previous_event_correlation_time=0;

  onlinemon->SetOnlineMon(this);    

}

RootMonitor::~RootMonitor(){
  gApplication->Terminate();
}

OnlineMonWindow* RootMonitor::getOnlineMon() const {
  return onlinemon;
}

void RootMonitor::setWriteRoot(const bool write) {
  _writeRoot = write;
}

void RootMonitor::setCorr_width(const unsigned c_w) {
  corrCollection->setWindowWidthForCorrelation(c_w);
}
void RootMonitor::setCorr_planes(const unsigned c_p) {
  corrCollection->setPlanesNumberForCorrelation(c_p);
}

void RootMonitor::setReduce(const unsigned int red) {
  if (_offline <= 0) onlinemon->setReduce(red);
  for (unsigned int i = 0 ; i < _colls.size(); ++i)
  {
    _colls.at(i)->setReduce(red);
  }
}


void RootMonitor::setUseTrack_corr(const bool t_c) {
  useTrackCorrelator = t_c;
}

bool RootMonitor::getUseTrack_corr() const {
  return useTrackCorrelator;
}

void RootMonitor::setTracksPerEvent(const unsigned int tracks) {
  tracksPerEvent = tracks;
}

unsigned int RootMonitor::getTracksPerEvent() const {
  return tracksPerEvent;
}

void RootMonitor::DoConfigure(){
  auto &param = *GetConfiguration();
  std::cout << "Configure: " << param.Name() << std::endl;
}

void RootMonitor::DoTerminate(){
  gApplication->Terminate();
}  

void RootMonitor::DoReceive(eudaq::EventSP evsp) {
  auto stdev = std::dynamic_pointer_cast<eudaq::StandardEvent>(evsp);
  if(!stdev){
    stdev = eudaq::StandardEvent::MakeShared();
    eudaq::StdEventConverter::Convert(evsp, stdev, nullptr); //no conf
  }

  uint32_t ev_plane_c = stdev->NumPlanes();
  if(m_ev_rec_n < 10){
    m_ev_rec_n ++;
    if(ev_plane_c > m_plane_c){
      m_plane_c = ev_plane_c;
    }
    return;
  }

  if(ev_plane_c != m_plane_c){
    std::cout<< "do nothing for this event at "<< evsp->GetEventN()<< ", ev_plane_c "<<ev_plane_c<<std::endl;
    return;
  }
  
  auto &ev = *(stdev.get());
  while(_offline <= 0 && onlinemon==NULL){
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
    
#ifdef DEBUG
  cout << "Called onEvent " << ev.GetEventNumber()<< endl;
  cout << "Number of Planes " << ev.NumPlanes()<< endl;
#endif
  _checkEOF.EventReceived();

  //    cout << "Called onEvent " << ev.GetEventNumber()<< endl;
  //start timing to measure processing time
  my_event_processing_time.Start(true);

  bool reduce=false; //do we use Event reduction
  bool skip_dodgy_event=false; // do we skip this event because we consider it dodgy

  if (_offline > 0) //are we in offline mode , activated with -o
  {
    if (_offline <(int)  ev.GetEventNumber())
    {
      TFile *f = new TFile(rootfilename.c_str(),"RECREATE");
      if (f!=NULL)
      {
        for (unsigned int i = 0 ; i < _colls.size(); ++i)
        {
          _colls.at(i)->Write(f);
        }
        f->Close();
      }
      else
      {
        cout<< "Can't open "<<rootfilename<<endl;
      }
      exit(0); // Kill the program
    }
    reduce = true;

  }
  else
  {
    reduce = (ev.GetEventNumber() % onlinemon->getReduce() == 0);
  }


  if (reduce)
  {
    unsigned int num = (unsigned int) ev.NumPlanes();
    // Initialize the geometry with the first event received:
    if(!_planesInitialized) {
      myevent.setNPlanes(num);
      std::cout << "Initialized geometry: " << num << " planes." << std::endl;
    }
    else {
      if (myevent.getNPlanes()!=num) {

        cout << "Plane Mismatch on " <<ev.GetEventNumber()<<endl;
        cout << "Current/Previous " <<num<<"/"<<myevent.getNPlanes()<<endl;
        skip_dodgy_event=false; //we may want to skip this FIXME
        ostringstream eudaq_warn_message;
        eudaq_warn_message << "Plane Mismatch in Event "<<ev.GetEventNumber() <<" "<<num<<"/"<<myevent.getNPlanes();
        EUDAQ_LOG(WARN,(eudaq_warn_message.str()).c_str());
	_planesInitialized = false;
	num = (unsigned int) ev.NumPlanes();
	eudaq_warn_message << "Continuing and readjusting the number of planes to  " << num;
	myevent.setNPlanes(num);
      }
      else {
        myevent.setNPlanes(num);
      }
    }

    SimpleStandardEvent simpEv;
    if ((ev.GetEventNumber() == 1) && (_offline <0)) //only update Display, when GUI is active
    {
      onlinemon->UpdateStatus("Getting data..");
    }
    // store the processing time of the previous EVENT, as we can't track this during the  processing
    simpEv.setMonitor_eventanalysistime(previous_event_analysis_time);
    simpEv.setMonitor_eventfilltime(previous_event_fill_time);
    simpEv.setMonitor_eventclusteringtime(previous_event_clustering_time);
    simpEv.setMonitor_eventcorrelationtime(previous_event_correlation_time);
    // add some info into the simple event header
    simpEv.setEvent_number(ev.GetEventNumber());
    simpEv.setEvent_timestamp(ev.GetTimestampBegin());
    
    std::string tagname;
    tagname = "Temperature";
    if(ev.HasTag(tagname)){
      double val;
      val = ev.GetTag(tagname, val);
      simpEv.setSlow_para(tagname,val);
    }
    tagname = "Voltage";
    if(ev.HasTag(tagname)){
      double val;
      val = ev.GetTag(tagname, val);
      simpEv.setSlow_para(tagname,val);
    }

    try{
    auto paralist = ev.GetTags();
    for(auto &e: paralist){
      if(e.first.find("PLOT_") == std::string::npos) continue;

      double val = stod(e.second);
      // val=ev.GetTag(e.first, val);
      // The histograms are booked in first event with some data?
      std::cout << "PLOT " << e.first << ": " << val << '\n';
      simpEv.setSlow_para(e.first,val);
    }
    } catch(...) {
      std::cout << "Failed to parse PLOT tag\n";
    }
    
    if (skip_dodgy_event)
    {
      return; //don't process any further
    }

    for (unsigned int i = 0; i < num;i++)
    {
      const eudaq::StandardPlane & plane = ev.GetPlane(i);

#ifdef DEBUG
      cout << "Plane ID         " << plane.ID()<<endl;
      cout << "Plane Size       " << sizeof(plane) <<endl;
      cout << "Plane Frames     " << plane.NumFrames() <<endl;
      for (unsigned int nframes=0; nframes<plane.NumFrames(); nframes++)
      {
        cout << "Plane Pixels Hit Frame " << nframes <<" "<<plane.HitPixels(0) <<endl;
      }
#endif

      string sensorname;
      if ((plane.Type() == std::string("DEPFET")) &&(plane.Sensor().length()==0)) // FIXME ugly hack for the DEPFET
      {
        sensorname=plane.Type();
      }
      else
      {
        sensorname=plane.Sensor();

      }
      // DEAL with Fortis ...
      if (strcmp(plane.Sensor().c_str(), "FORTIS") == 0 )
      {
        continue;
      }
      SimpleStandardPlane simpPlane(sensorname,plane.ID(),plane.XSize(),plane.YSize(),&mon_configdata);

      for (unsigned int lvl1 = 0; lvl1 < plane.NumFrames(); lvl1++)
      {
        // if (lvl1 > 2 && plane.HitPixels(lvl1) > 0) std::cout << "LVLHits: " << lvl1 << ": " << plane.HitPixels(lvl1) << std::endl;

        for (unsigned int index = 0; index < plane.HitPixels(lvl1);index++)
        {
          SimpleStandardHit hit((int)plane.GetX(index,lvl1),(int)plane.GetY(index,lvl1));
          hit.setTOT((int)plane.GetPixel(index,lvl1)); //this stores the analog information if existent, else it stores 1
          hit.setLVL1(lvl1);



          if (simpPlane.getAnalogPixelType()) //this is analog pixel, apply threshold
          {
            if (simpPlane.is_DEPFET)
            {
              if ((hit.getTOT()< -20) || (hit.getTOT()>120))
              {
                continue;
              }
            }
	    if (simpPlane.is_EXPLORER)
	    {
	      if (lvl1!=0) continue;
	      hit.setTOT((int)plane.GetPixel(index));
	      //if (hit.getTOT() < -20 || hit.getTOT() > 20) {
		//std::cout << hit.getTOT() << std::endl;
	      //}
	      if (hit.getTOT() < 20)
	      {
		continue;
	      }
	    }
            simpPlane.addHit(hit);
          }
          else //purely digital pixel
          {
            simpPlane.addHit(hit);
          }

        }
      }
      simpEv.addPlane(simpPlane);
#ifdef DEBUG
      cout << "Type: " << plane.Type() << endl;
      cout << "StandardPlane: "<< plane.Sensor() <<  " " << plane.ID() << " " << plane.XSize() << " " << plane.YSize() << endl;
      cout << "PlaneAddress: " << &plane << endl;
#endif

    }

    my_event_inner_operations_time.Start(true);
    simpEv.doClustering();
    my_event_inner_operations_time.Stop();
    previous_event_clustering_time = my_event_inner_operations_time.RealTime();

    if(!_planesInitialized)
    {
#ifdef DEBUG
      cout << "Waiting for booking of Histograms..." << endl;
#endif
      std::this_thread::sleep_for(std::chrono::seconds(1));
#ifdef DEBUG
      cout << "...long enough"<< endl;
#endif
      _planesInitialized = true;
    }

    //stop the Stop watch
    my_event_processing_time.Stop();
#ifdef DEBUG
    cout << "Analysing"<<   " "<< my_event_processing_time.RealTime()<<endl;
#endif
    previous_event_analysis_time=my_event_processing_time.RealTime();
    //Filling
    my_event_processing_time.Start(true); //start the stopwatch again
    for (unsigned int i = 0 ; i < _colls.size(); ++i)
    {
      if (_colls.at(i) == corrCollection)
      {
        my_event_inner_operations_time.Start(true);
        if (getUseTrack_corr() == true)
        {
          tracksPerEvent = corrCollection->FillWithTracks(simpEv);
          if (eudaqCollection->getEUDAQMonitorHistos() != NULL) //workaround because Correlation Collection is before EUDAQ Mon collection
            eudaqCollection->getEUDAQMonitorHistos()->Fill(simpEv.getEvent_number(), tracksPerEvent);

        }
        else
          _colls.at(i)->Fill(simpEv);
        my_event_inner_operations_time.Stop();
        previous_event_correlation_time = my_event_inner_operations_time.RealTime();
      }
      else
        _colls.at(i)->Fill(simpEv);

      // CollType is used to check which kind of Collection we are having
      if (_colls.at(i)->getCollectionType()==HITMAP_COLLECTION_TYPE) // Calculate is only implemented for HitMapCollections
      {
        _colls.at(i)->Calculate(ev.GetEventNumber());
      }
    }

    if (_offline <= 0)
    {
      onlinemon->setEventNumber(ev.GetEventNumber());
      onlinemon->increaseAnalysedEventsCounter();
    }
  } // end of reduce if
  my_event_processing_time.Stop();
#ifdef DEBUG
  cout << "Filling " << " "<< my_event_processing_time.RealTime()<<endl;
  cout << "----------------------------------------"  <<endl<<endl;
#endif
  previous_event_fill_time=my_event_processing_time.RealTime();

  if (ev.IsBORE())
  {
    std::cout << "This is a BORE" << std::endl;
  }

}

void RootMonitor::autoReset(const bool reset) {
  //_autoReset = reset;
  if (_offline <= 0) onlinemon->setAutoReset(reset);

}

void RootMonitor::DoStopRun()
{
  m_plane_c = 0;
  m_ev_rec_n = 0;
  while(_offline <= 0 && onlinemon==NULL){
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (_writeRoot)
  {
    TFile *f = new TFile(rootfilename.c_str(),"RECREATE");
    for (unsigned int i = 0 ; i < _colls.size(); ++i)
    {
      _colls.at(i)->Write(f);
    }
    f->Close();
  }
  onlinemon->UpdateStatus("Run stopped");
}

void RootMonitor::DoStartRun() {
  m_plane_c = 0;
  m_ev_rec_n = 0;
  uint32_t param = GetRunNumber();
  while(_offline <= 0 && onlinemon==NULL){
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (onlinemon->getAutoReset())
  {
    onlinemon->UpdateStatus("Resetting..");
    for (unsigned int i = 0 ; i < _colls.size(); ++i)
    {
      if (_colls.at(i) != NULL)
        _colls.at(i)->Reset();
    }
  }

  std::cout << "Called on start run" << param <<std::endl;
  onlinemon->UpdateStatus("Starting run..");
  char out[255];
  sprintf(out, "run%d.root",param);
  rootfilename = std::string(out);
  runnumber = param;

  if (_offline <= 0) {
    onlinemon->setRunNumber(runnumber);
    onlinemon->setRootFileName(rootfilename);
  }

  // Reset the planes initializer on new run start:
  _planesInitialized = false;
}

void RootMonitor::setUpdate(const unsigned int up) {
  if (_offline <= 0) onlinemon->setUpdate(up);
}


//sets the location for the snapshots
void RootMonitor::SetSnapShotDir(string s)
{
  snapshotdir=s;
}


//gets the location for the snapshots
string RootMonitor::GetSnapShotDir()const{
  return snapshotdir;
}

int main(int argc, const char ** argv) {

  eudaq::OptionParser op("EUDAQ Root Monitor", "1.0", "A Monitor using root for gui and graphics");
  eudaq::Option<std::string> rctrl(op, "r", "runcontrol", "tcp://localhost:44000", "address",
      "The address of the RunControl application");
  eudaq::Option<std::string> level(op, "l", "log-level", "NONE", "level",
      "The minimum level for displaying log messages locally");
  eudaq::Option<int>             x(op, "x", "left",    100, "pos");
  eudaq::Option<int>             y(op, "y", "top",       0, "pos");
  eudaq::Option<int>             w(op, "w", "width",  1400, "pos");
  eudaq::Option<int>             h(op, "g", "height",  700, "pos", "The initial position of the window");
  eudaq::Option<int>             reduce(op, "rd", "reduce",  1, "Reduce the number of events");
  eudaq::Option<unsigned>        corr_width(op, "cw", "corr_width",500, "Width of the track correlation window");
  eudaq::Option<unsigned>        corr_planes(op, "cp", "corr_planes",  5, "Minimum amount of planes for track reconstruction in the correlation");
  eudaq::Option<bool>            track_corr(op, "tc", "track_correlation", false, "Using (EXPERIMENTAL) track correlation(true) or cluster correlation(false)");
  eudaq::Option<int>             update(op, "u", "update",  1000, "update every ms");
  eudaq::Option<int>             offline(op, "o", "offline",  0, "running is offlinemode - analyse until event <num>");
  eudaq::Option<std::string>     configfile(op, "c", "config_file"," ", "filename","Config file to use for onlinemon");
  eudaq::Option<std::string>     monitorname(op, "t", "monitor_name"," ", "StdEventMonitor","Name for onlinemon");	
  eudaq::OptionFlag do_rootatend (op, "rf","root","Write out root-file after each run");
  eudaq::OptionFlag do_resetatend (op, "rs","reset","Reset Histograms when run stops");

  try {
    op.Parse(argv);
    EUDAQ_LOG_LEVEL(level.Value());

    if (!rctrl.IsSet()) rctrl.SetValue("null://");
    TApplication theApp("App", &argc, const_cast<char**>(argv),0,0);
    RootMonitor mon(rctrl.Value(),
		    x.Value(), y.Value(), w.Value(), h.Value(),
		    argc, offline.Value(), configfile.Value(),monitorname.Value());
    mon.setWriteRoot(do_rootatend.IsSet());
    mon.autoReset(do_resetatend.IsSet());
    mon.setReduce(reduce.Value());
    mon.setUpdate(update.Value());
    mon.setCorr_width(corr_width.Value());
    mon.setCorr_planes(corr_planes.Value());
    mon.setUseTrack_corr(track_corr.Value());

    cout <<"Monitor Settings:" <<endl;
    cout <<"Update Interval :" <<update.Value() <<" ms" <<endl;
    cout <<"Reduce Events   :" <<reduce.Value() <<endl;
    //TODO: run cmd data thread
    eudaq::Monitor *m = dynamic_cast<eudaq::Monitor*>(&mon);
    m->Connect();
    theApp.Run(); //execute
  } catch (...) {
    return op.HandleMainException();
  }
  return 0;
}
