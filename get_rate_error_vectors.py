#!/usr/bin/python

import sys
import os
import time
import subprocess

ideal_rates = {}
xf_rates = {}
findex=1
fstartindex=3
fsizeindex=5

def small_flow(flow_size):
    if(flow_size<100000):
        return true
    return false

def medium_flow(flow_size):
    if(flow_size >= 100000 and flow_size <1000000):
        return true
    return false

error_correction = 0.016 + 0.008  #SYN-SYNACK + 1/2 RTT PROPAGATION

prefix=sys.argv[1]
ns3_log=prefix+".out"
ideal_fcts=prefix+".ideal"

fstarts = {}
fsizes = {}
fstops = {}
f1 = open(ns3_log, 'r')
for line in f1:
  elems = []
  elems = (line.rstrip()).split(' ')

  if(elems[0] == "flow_start"):
    fid = int(elems[findex])
    fstart =  float(elems[fstartindex])
    fsize = float(elems[fsizeindex])
  
    fstarts[fid] = fstart
    fsizes[fid] = fsize

  if(elems[0] == "flow_stop"):
    fid = int(elems[findex])
    fstop =  float(elems[fstartindex])
    fsize = float(elems[fsizeindex])

    fstops[fid] = fstop

# get ideal fcts first
#ideal_rate_cmd = "python extract_graph_pfabric.py "+ns3_log+" mp > "+ideal_fcts
#proc = subprocess.Popen(ideal_rate_cmd, shell="False")
#proc.wait()
#time.sleep(5)

# make sure the file is written by sleeping 

f = open(ideal_fcts,'r')
for line in f:
  elems = []
  elems = (line.rstrip()).split(' ')
  if(elems[0] == "removing"):
    fid = int(elems[6])
    time_taken = (float(elems[11]) + error_correction)/1000  # get it in seconds
    flow_size = int(elems[13])
    print("flow %d fct %f" %(fid,time_taken))
    ideal_rates[fid] = flow_size/(1000000000.0*time_taken) # GBps
    flow_sizes[fid] = flow_size
error = {}

small_errors = open(sys.argv[1]+"_small_errors", "w")
medium_errors = open(sys.argv[1]+"_medium_errors", "w")
large_errors = open(sys.argv[1]+"_large_errors", "w")

for key in fstarts:
  if(key in fstops):
    ftime = (fstops[key] - fstarts[key])
    #print("Flow %d Size %d bytes finished in %f normalized %f" %(key, fsizes[key], ftime, ftime/fsizes[key]))
    xf_rates[key] = fsizes[key]/ftime
    #... and compare..
    error[key] = (xf_rates[key]-ideal_rates[key])/ideal_rates[key]
    #... where to write?
    if(small_flow(flow_sizes[fid])):
        small_errors.write("%d %d %f %f %f\n" %(key, flow_sizes[fid],xf_rates[key], ideal_rates[key],error[key]))
    elif(medium_flow(flow_sizes[fid])):
        medium_errors.write("%d %d %f %f %f\n" %(key, flow_sizes[fid],xf_rates[key], ideal_rates[key],error[key]))
    else:
        large_errors.write("%d %d %f %f %f\n" %(key, flow_sizes[fid],xf_rates[key], ideal_rates[key],error[key]))
    print("ERROR fid %d error %f xf %f ideal %f" %(key,error[key],xf_rates[key],ideal_rates[key]))
  else:
    print("Flow %d started at %f size %f did not stop" %(key, fsizes[key], fstarts[key]))


  
  
