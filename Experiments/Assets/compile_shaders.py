import argparse
import os
import subprocess

def compileShader(input, output, shaderType, entryPoint):
    if(os.path.exists(output) and os.path.getmtime(input) < os.path.getmtime(output)):
        print("Shader Compiler: Input file older than built file.")
        return

    scriptDir = os.path.dirname(os.path.abspath(__file__))

    shaderType = ""
    if(args.type.lower() == "compute"):
        shaderType = "cs_6_6"
    elif(args.type.lower() == "pixel"):
        shaderType = "ps_6_6"
    elif(args.type.lower() == "vertex"):
        shaderType = "vs_6_6"

    dxcArgs = [f"{scriptDir}/../../dxc/bin/dxc.exe",
                    "-spirv",
                    f"-T {shaderType}",
                    f"-E {entryPoint}",
                    "-fspv-target-env=vulkan1.3",
                    "-WX",
                    "-O3",
                    "-enable-16bit-types",
                    "-HV 202x",
                    "-Zpr",
                    "-Ges",
                    "-Wconversion",
                    "-all-resources-bound",
                    f"-Fo {output}",
                    input]

    compiledAssetPath = os.path.dirname(args.output)
    os.makedirs(compiledAssetPath, exist_ok=True)

    commandLine=" ".join(dxcArgs)
    print(f"Shader Compiler: {commandLine}")
    procResult = subprocess.run(commandLine, capture_output=True)

    stdout = procResult.stdout.decode("utf-8")
    stderr = procResult.stderr.decode("utf-8")
    if(stdout):
        print(f"Shader Compiler: { stdout }")
    if(stderr):
        print(f"Shader Compiler: { stderr }")

if __name__ == "__main__":
    parser = argparse.ArgumentParser("Shader Compilation")
    parser.add_argument("-i", "--input", help="Path to input files")
    parser.add_argument("-e", "--entry_point", help="Shader Entry Point")
    parser.add_argument("-t", "--type", help="Shader Type")
    parser.add_argument("-o", "--output", help="Output path")
    args = parser.parse_args()

    compileShader(args.input, args.output, args.type, args.entry_point)