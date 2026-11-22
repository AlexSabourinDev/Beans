import os
import subprocess

scriptDir = os.path.dirname(os.path.abspath(__file__))

def generateShaderHeader(input):
    basename = os.path.basename(input)
    filename, ext = os.path.splitext(basename)

    shaderNameList = filename.split("_")
    for i in range(len(shaderNameList)):
        shaderNameList[i] = shaderNameList[i].capitalize()
    shaderName = "".join(shaderNameList)

    codegenArgs = [f"{scriptDir}/../x64/Debug/ShaderHeaderGenerator.exe",
                               f"-i = {scriptDir}/../CompiledAssets/Shaders/{filename}.spv",
                               f"-o = {scriptDir}/../ShaderHeaders/{filename}.h",
                               f"-n = {shaderName}"]

    codegenArgs[1] = codegenArgs[1].replace("\\", "/")
    codegenArgs[2] = codegenArgs[2].replace("\\", "/")

    commandLine = " ".join(codegenArgs)
    
    print(f"Generating {filename}.h")
    subprocess.run(commandLine, capture_output=True)


if __name__ == "__main__":
    if not os.path.exists(f"{scriptDir}/../ShaderHeaders/"):
        os.mkdir(f"{scriptDir}/../ShaderHeaders/")

    for e in os.scandir(f"{scriptDir}/../Assets/Shaders"):
        if e.is_file():
            basename = os.path.basename(e.path)
            filename, ext = os.path.splitext(basename)
            if ext == ".hlsl":
                  generateShaderHeader(e.path)

