from generate_shader_headers import generateShaderHeader
from compile_shaders import compileShader
import os

def generateShader(input, output, type, entry):
    scriptDir = os.path.dirname(os.path.abspath(__file__))
    compileShader(f"{scriptDir}/{input}", f"{scriptDir}/{output}", type, entry)
    generateShaderHeader(f"{scriptDir}/{input}")

generateShader("../VoxelGame/Shaders/background.hlsl", "../VoxelGame/CompiledAssets/Shaders/background.hlsl", "compute", "CS")