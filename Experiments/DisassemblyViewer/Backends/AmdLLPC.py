import subprocess
import os
import argparse
import csv

GPU = "gfx1030"

def runProc(args):
    commandLine=" ".join(args)
    print(commandLine)
    procResult = subprocess.run(commandLine, capture_output=True)
    print(procResult.stdout.decode("utf-8"))
    print(procResult.stderr.decode("utf-8"))

def deleteIfExists(path):
    if os.path.exists(path):
        os.remove(path)

if __name__ == "__main__":

    parser = argparse.ArgumentParser("Shader Compilation")
    parser.add_argument("-i", "--input", help="Path to input file")
    parser.add_argument("-o", "--output", help="Path to output file")
    parser.add_argument("-e", "--entry_point", help="Shader Entry Point")
    args = parser.parse_args()
    
    fileDir = os.path.dirname(os.path.abspath(__file__))
    spvFile="temp/temp_shader.spv"
    runProc([f"{fileDir}/dxc/dxc.exe",
                    "-spirv",
                    "-T cs_6_6",
                    f"-E {args.entry_point}",
                    "-fspv-target-env=vulkan1.3",
                    "-WX",
                    "-O3",
                    "-enable-16bit-types",
                    "-HV 2021",
                    "-Zpr",
                    f"-Fo {spvFile}",
                    args.input])
    
    rgaShaderTye="comp"
    analysisFile="temp/temp_analysis.txt"
    runProc([f"{fileDir}/rga/rga.exe",
                    "-s vk-offline",
                    f"-c {GPU}",
                    f"-a {analysisFile}",
                    f"--{rgaShaderTye}",
                    spvFile])
    
    statsOutput=[]

    realAnalysisFileName=f"temp/{GPU}_temp_analysis_{rgaShaderTye}.txt"
    with open(realAnalysisFileName) as csvfile:
        reader = csv.DictReader(csvfile)
        readerList = list(reader)
        statsOutput.append(f"VGPRs:{readerList[0]['USED_VGPRs']}")
        statsOutput.append(f"SGPRs:{readerList[0]['USED_SGPRs']}")
        statsOutput.append(f"LDS:{readerList[0]['USED_LDS_BYTES']}")
    
    with open("temp/stats.txt", "w") as f:
        f.write("\n".join(statsOutput))

    deleteIfExists(spvFile)
    deleteIfExists(analysisFile)
    deleteIfExists(realAnalysisFileName)
        