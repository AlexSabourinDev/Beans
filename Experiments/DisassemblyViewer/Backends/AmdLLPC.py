import subprocess
import os
import argparse
import csv

GPU = "gfx1030"
AMDLLPCGPU = "10.3.0"
SHAEGPU="gfx10_3"

def runProc(args, outPipe=None):
    captureOutput = (outPipe == None)

    commandLine=" ".join(args)
    print(commandLine)
    procResult = subprocess.run(commandLine, capture_output=captureOutput, stdout=outPipe)

    if(captureOutput):
        print(procResult.stdout.decode("utf-8"))
        print(procResult.stderr.decode("utf-8"))

def deleteIfExists(path):
    if os.path.exists(path):
        os.remove(path)

if __name__ == "__main__":

    cwd = os.getcwd()

    parser = argparse.ArgumentParser("Shader Compilation")
    parser.add_argument("-i", "--input", help="Path to input file")
    parser.add_argument("-o", "--output", help="Path to output file")
    parser.add_argument("-s", "--stats", help="Path to stats file")
    parser.add_argument("-e", "--entry_point", help="Shader Entry Point")
    parser.add_argument("--debug_info", help="Debug Info Type", choices=["none", "vulkan", "opencl"], default="vulkan")
    parser.add_argument("--liveness", help="Analyse Liveness - Loses Debug Info :(")
    args = parser.parse_args()
    
    fileDir = os.path.dirname(os.path.abspath(__file__))
    spvFile=f"{cwd}/temp/temp_shader.spv"
    
    dxcArgs = [f"{fileDir}/dxc/dxc.exe",
                    "-spirv",
                    "-T cs_6_6",
                    f"-E {args.entry_point}",
                    "-fspv-target-env=vulkan1.3",
                    "-WX",
                    "-O3",
                    "-enable-16bit-types",
                    "-HV 2021",
                    "-Zpr",
                    f"-Fo {spvFile}"]

    if(args.debug_info != "none"):
        dxcArgs.append("-Zi")
        dxcArgs.append("-Qembed_debug")

    if(args.debug_info == "vulkan"):
        dxcArgs.append("-fspv-debug=vulkan")
    elif(args.debug_info == "opencl"):
        dxcArgs.append("-fspv-debug=rich")

    dxcArgs.append(args.input)
    runProc(dxcArgs)

    rgaShaderTye="comp"
    analysisFile=f"{cwd}/temp/temp_analysis.txt"
    runProc([f"{fileDir}/rga/rga.exe",
                    "-s vk-offline",
                    f"-c {GPU}",
                    f"-a {analysisFile}",
                    f"--{rgaShaderTye}",
                    spvFile])
    
    statsOutput=[]

    realAnalysisFileName=f"{cwd}/temp/{GPU}_temp_analysis_{rgaShaderTye}.txt"
    with open(realAnalysisFileName) as csvfile:
        reader = csv.DictReader(csvfile)
        readerList = list(reader)
        statsOutput.append(f"VGPRs:{readerList[0]['USED_VGPRs']}")
        statsOutput.append(f"SGPRs:{readerList[0]['USED_SGPRs']}")
        statsOutput.append(f"LDS:{readerList[0]['USED_LDS_BYTES']}")
    
    with open(args.stats, "w") as f:
        f.write("\n".join(statsOutput))

    shaderBinary=f"{cwd}/temp/temp_shader_binary.bin"
    runProc([f"{fileDir}/rga/utils/amdllpc.exe",
                    "--auto-layout-desc", # TODO: Some shaders crash amdllpc if this is enabled!
                    f"-o={shaderBinary}",
                    f"--gfxip={AMDLLPCGPU}", # TODO: Make configurable
                    "-trim-debug-info=false",
                    "--dwarf-inlined-strings=Enable",
                    spvFile])

    if(args.liveness):
        # use amdgpu-dis for register analysis.
        # shae doesn't like the disassembly from objdump.
        tempDisassemblyPath=f"{cwd}/temp/temp_amd_dissassembly.txt"
        runProc([f"{fileDir}/rga/utils/lc/disassembler/amdgpu-dis.exe",
            f"-o {tempDisassemblyPath}",
            shaderBinary])

        runProc([f"{fileDir}/rga/utils/shae.exe",
            f"-i {SHAEGPU}",
            "analyse-liveness",
            tempDisassemblyPath,
            args.output])

        deleteIfExists(tempDisassemblyPath)
    else:
        with open(args.output, "w+") as f:
            runProc([f"{fileDir}/rga/utils/lc/opencl/bin/llvm-objdump.exe",
                "--disassemble",
                "--symbolize-operands",
                "--line-numbers",
                "--source",
                "--triple=amdgcn--amdpal",
                f"--mcpu={GPU}",
                shaderBinary], f)

    deleteIfExists(shaderBinary)
    deleteIfExists(spvFile)
    deleteIfExists(analysisFile)
    deleteIfExists(realAnalysisFileName)