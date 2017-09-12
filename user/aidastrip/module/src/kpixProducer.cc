#include "eudaq/Producer.hh"
#include <iostream>
#include <fstream>
#include <ratio>
#include <chrono>
#include <thread>
#include <random>
#ifndef _WIN32
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include <signal.h>
//--> start of kpix libs:
#include "KpixControl.h"
#include "UdpLink.h"
#include "System.h"
#include "ControlServer.h"
#include "Data.h"
//--> end of kpix libs:

//----------DOC-MARK-----BEG*DEC-----DOC-MARK----------
//class System;

class kpixProducer : public eudaq::Producer {
  public:
  kpixProducer(const std::string & name, const std::string & runcontrol);
  void DoInitialise() override;
  void DoConfigure() override;
  void DoStartRun() override;
  void DoStopRun() override;
  void DoTerminate() override;
  void DoReset() override;
  
  void OnStatus() override;
  
  void Mainloop();
  Data* pollKpixData(int &evt_counter);
    
  static const uint32_t m_id_factory = eudaq::cstr2hash("kpixProducer");
  bool stop; //kpixdev
   
private:
 
  UdpLink udpLink;
  std::string m_defFile;
  std::string m_debug;
  std::string m_kpixRunState;
  //KpixControl kpix(&udpLink, m_defFile, 32);
  KpixControl *kpix;
  //int m_port; 

  std::string m_runrate, m_dataOverEvt;

  bool m_exit_of_run;
  std::thread m_thd_run;

  uint32_t m_runcount;
  //bool m_noDevice;
};
//----------DOC-MARK-----END*DEC-----DOC-MARK---------

//----------DOC-MARK-----BEG*REG-----DOC-MARK----------
namespace{
  auto dummy0 = eudaq::Factory<eudaq::Producer>::
    Register<kpixProducer, const std::string&, const std::string&>(kpixProducer::m_id_factory);
}
//----------DOC-MARK-----END*REG-----DOC-MARK----------

//----------DOC-MARK-----BEG*CON-----DOC-MARK----------
kpixProducer::kpixProducer(const std::string & name, const std::string & runcontrol)
  :eudaq::Producer(name, runcontrol),
   m_debug("False"), m_kpixRunState("Running"),
   m_runrate("1Hz"), m_dataOverEvt("0 - 0 Hz")
{
  // Create and setup PGP link
  udpLink.setMaxRxTx(500000);
  udpLink.open(8192,1,"192.168.1.16"); // to have multiple udplink connections
  //udpLink.openDataNet("127.0.0.1",8099);
  //usleep(100);
}

//----------DOC-MARK-----BEG*INI-----DOC-MARK----------
void kpixProducer::DoInitialise(){
  
  auto ini = GetInitConfiguration();
  // --- read the ini file:
  m_defFile = ini->Get("KPIX_CONFIG_FILE", "defaults.xml"); //kpixdev
  m_debug = ini->Get("KPIX_DEBUG", "False");
  //  m_noDevice = true; // if no device connected, to test sturcture
  
  try{
    if (m_debug!="True" && m_debug!="False")
      throw std::string("      KPIX_DEBUG value error! (must be 'True' or 'False')\n");
    else std::cout<<"[Init:info] kpix system debug == "<< m_debug <<std::endl;
    bool b_m_debug = (m_debug=="True")? true : false;
  
    kpix =new KpixControl(&udpLink, m_defFile, 32);
    kpix->setDebug(b_m_debug);

    udpLink.setDebug(b_m_debug);
    udpLink.enableSharedMemory("kpix",1);

    //kpix->poll(NULL);

    std::string xmlString="<system><command><ReadStatus/>\n</command></system>";

    kpix->poll(NULL);
    kpix->parseXmlString(xmlString);
    
  }catch(std::string error) {
    std::cout <<"Caught error: "<<std::endl;
    std::cout << error << std::endl;
  }

  
}

//----------DOC-MARK-----BEG*CONF-----DOC-MARK----------
void kpixProducer::DoConfigure(){
  auto conf = GetConfiguration();
  conf->Print(std::cout);
  
  std::string conf_defFile = conf->Get("KPIX_CONFIG_FILE","");
  if (conf_defFile!="") m_defFile=conf_defFile; 
  kpix->parseXmlFile(m_defFile); // work as set defaults

  //--> read eudaq config to rewrite the kpix config:
  //--> Kpix Config override:
  std::string database = conf->Get("KPIX_DataBase","");
  std::string datafile = conf->Get("KPIX_DataFile","");
  std::string dataauto = conf->Get("KPIX_DataAuto","");
  if (database!="") kpix->getVariable("DataBase")->set(database); 
  if (datafile!="") kpix->getVariable("DataFile")->set(datafile);
  if (dataauto!="") kpix->getVariable("DataAuto")->set(dataauto);
  
  //--> Kpix Run Control
  m_kpixRunState = conf->Get("KPIX_RunState","Running");
  m_runcount = conf->Get("KPIX_RunCount",0);
  // if (m_runcount!=0) kpix->getVariable("RunCount")->setInt(m_runcount);
  // kpix->getVariable("RunCount")->setInt(2);
  std::cout<<"[producer:dev] m_runcount = "<< m_runcount <<std::endl;  
  std::cout<<"[producer:dev] run count = "<< kpix->getVariable("RunCount")->getInt() <<std::endl;
  //m_runcount = kpix->getVariable("RunCount")->getInt();
  std::cout<<"[producer:dev] run progress = "<<kpix->getVariable("RunProgress")->getInt() <<std::endl;
  
  m_runrate = kpix->getVariable("RunRate")->get();
  std::cout<<"[producer:dev] run_rate = "<< m_runrate <<std::endl;
}
//----------DOC-MARK-----BEG*RUN-----DOC-MARK----------
void kpixProducer::DoStartRun(){
  //kpix->command("OpenDataFile","");
  m_thd_run = std::thread(&kpixProducer::Mainloop, this);
}
//----------DOC-MARK-----BEG*STOP-----DOC-MARK----------
void kpixProducer::DoStopRun(){
  /* Func1: stop the Kpix*/
  //kpix->command("CloseDataFile","");
  //kpix->command("SetRunState","Stopped");

  /* Func2: stop the Mainloop*/
  m_exit_of_run = true;
  if(m_thd_run.joinable()){
    m_thd_run.join();
  }

  
}
//----------DOC-MARK-----BEG*RST-----DOC-MARK----------
void kpixProducer::DoReset(){
  kpix->parseXmlString("<system><command><HardReset/></command></system>");

  /* Step1: stop Mainloop (kpix+data collecting)*/
  m_exit_of_run = true;
  if(m_thd_run.joinable())
    m_thd_run.join();

  /* Step2: set the thread free and turned off the exit_of_run sign*/
  m_thd_run = std::thread();
  m_exit_of_run = false;
}
//----------DOC-MARK-----BEG*TER-----DOC-MARK----------
void kpixProducer::DoTerminate(){
  //-->you can call join to the std::thread in order to set it non-joinable to be safely destroyed
  /* Func1: stop Mainloop (kpix+data collecting)*/
  m_exit_of_run = true;
  if(m_thd_run.joinable())
    m_thd_run.join();

  //kpix->command("CloseDataFile",""); // no problem though file closed, as protected in kpix/generic/System.cpp

  /* stop listen to the hardware */
  udpLink.close();
  
  delete kpix;

}

//----------DOC-MARK-----BEG*STATUS-----DOC-MARK----------
void kpixProducer::OnStatus(){
  /* Func:
   * SetStatusTag from CommandReceiver;
   * Tags collecting and transferred to euRun GUI as the value of tcp key of map_conn_status.
  */
  SetStatusTag("Status", "test");
  SetStatusTag("Data/Event", m_dataOverEvt);
  SetStatusTag("Run Rate", m_runrate);
  SetStatusTag("Configuration Tab","conf. values computed from .config" );
  SetStatusTag("KpixStopped", m_exit_of_run ? "true" : "false");
}

//----------DOC-MARK-----BEG*LOOP-----DOC-MARK----------


void kpixProducer::Mainloop(){
  /*Open data file to write*/
  kpix->command("OpenDataFile","");
  
  kpix->command("SetRunState",m_kpixRunState);

  auto tp_start_run = std::chrono::steady_clock::now();
  bool save_ts=true;

  int evt_counter = 0;

  // for (int ii=10; ii>=0; ii--){
  for (int ii = m_runcount; ii>=0; ii--){
    /* sleep 1 second after each loop */
    auto tp_current_evt = std::chrono::steady_clock::now();
    auto tp_next = tp_current_evt +  std::chrono::seconds(1);
    std::this_thread::sleep_until(tp_next);
    
    
    kpix->poll(NULL);

    auto btrial = kpix->getVariable("RunState")->get();
    std::cout<<"\t[KPiX:dev] Run State@mainLoop = "<<btrial<<std::endl;

    auto dataOverEvt = kpix->getVariable("DataRxCount")->get();
    std::cout<< "\t[KPiX:dev] Data/Event ==> " << dataOverEvt <<std::endl;
    m_dataOverEvt=dataOverEvt;

    /* start wmq-dev: polling data from kpix to eudaq*/
    auto ev = eudaq::Event::MakeUnique("KpixRawEvt");
    auto databuff = pollKpixData(evt_counter);

    //--> data is ready
    if (databuff!=NULL) {
      auto tp_trigger = std::chrono::steady_clock::now();
      
      if (save_ts) {
	std::chrono::nanoseconds this_ts_ns(tp_trigger - tp_start_run);
	ev->SetTimestamp(this_ts_ns.count(), this_ts_ns.count());
	std::cout<<"\t[Loop]: Timestamp enabled to use\n"; //wmq
	std::cout<<"\t[Loop]: time stamp at ==> "<<this_ts_ns.count()<<"ns\n";
      }
      auto buff = databuff->data();
      auto size = databuff->size();
      std::vector<Data*> dataToSave = {databuff};

      std::cout<<"[producer.CHECK] buff = "<< buff << "\n"
	       <<"                 size = "<< size << std::endl;

      //--> milestone: remember this pointer stuff @ August-17th
      ev->AddBlock(0, buff, size*4);

      delete databuff;
      SendEvent(std::move(ev));
    }
    /* end wmq-dev: polling data from kpix to eudaq*/
   

    if (btrial =="Stopped") {
      m_exit_of_run = true;
      break;
    }
    else if (m_exit_of_run){
      kpix->command("SetRunState","Stopped");
      break;
    } else/*do nothing*/;

  }

  std::cout<<"FINISH: kpix finished running"<<std::endl;
  std::cout<<"\t m_exit_of_run == "<< (m_exit_of_run ? "true" : "false") <<std::endl;
  /*Close data file to write*/
  kpix->command("CloseDataFile","");

  /* TODO: Do sth to get a stopped sign*/
}

/* start wmq-dev: polling data from kpix to eudaq*/
Data* kpixProducer::pollKpixData(int &evt_counter){

  //if (evt_counter>=10) return; // a safety check
  
  // int evt_counter = 0;
  // while(!m_exit_of_datapoll && !m_exit_of_run){
  auto datbuff = udpLink.pollDataQueue(0);
  if (datbuff != NULL) {
    std::cout<< " @_@ COUNTER! Event #"<< evt_counter<<std::endl;
    evt_counter++;
  }
    //}
  return datbuff;
  
}/* end wmq-dev: polling data from kpix to eudaq*/


//----------DOC-MARK-----END*IMP-----DOC-MARK----------

