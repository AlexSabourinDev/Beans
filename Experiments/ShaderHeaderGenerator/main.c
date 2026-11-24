#include <spirv_reflect.h>
#define SOKOL_IMPL
#include <sokol/sokol_args.h>

#include <stdio.h>
#include <string.h>

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

static void readWholeFileFromHandle(FILE* file, char** output, size_t* outputSize)
{
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET); /* same as rewind(f); */

    *output = realloc(*output, fileSize);
    *outputSize = fileSize;

    fileSize = fread(*output, 1, fileSize, file);
}

static bool readWholeFile(char const* path, char** output, size_t* outputSize)
{
    FILE *file = fopen(path, "rb");
    if (file != NULL)
    {
        readWholeFileFromHandle(file, output, outputSize);
        fclose(file);
        return true;
    }

    return false;
}

static char const* descriptorTypeToString(SpvReflectDescriptorType type)
{
    switch (type)
    {
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
            return "VK_DESCRIPTOR_TYPE_SAMPLER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            return "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            return "VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            return "VK_DESCRIPTOR_TYPE_STORAGE_IMAGE";
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return "VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return "VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER";
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            return "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC";
        case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return "VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT";
        case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return "VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR";
    }

    return "";
}

static char const* shaderModuleToString(SpvReflectShaderStageFlagBits stage)
{
    switch (stage)
    {
        case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:
            return "VK_SHADER_STAGE_VERTEX_BIT";
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
            return "VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT";
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
            return "VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT";
        case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT:
            return "VK_SHADER_STAGE_GEOMETRY_BIT";
        case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:
            return "VK_SHADER_STAGE_FRAGMENT_BIT";
        case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:
            return "VK_SHADER_STAGE_COMPUTE_BIT";
        case SPV_REFLECT_SHADER_STAGE_TASK_BIT_EXT:
            return "VK_SHADER_STAGE_TASK_BIT_EXT";
        case SPV_REFLECT_SHADER_STAGE_MESH_BIT_EXT:
            return "VK_SHADER_STAGE_MESH_BIT_EXT";
        case SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR:
            return "VK_SHADER_STAGE_RAYGEN_BIT_KHR";
        case SPV_REFLECT_SHADER_STAGE_ANY_HIT_BIT_KHR:
            return "VK_SHADER_STAGE_ANY_HIT_BIT_KHR";
        case SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            return "VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR";
        case SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR:
            return "VK_SHADER_STAGE_MISS_BIT_KHR";
        case SPV_REFLECT_SHADER_STAGE_INTERSECTION_BIT_KHR:
            return "VK_SHADER_STAGE_INTERSECTION_BIT_KHR";
        case SPV_REFLECT_SHADER_STAGE_CALLABLE_BIT_KHR:
            return "VK_SHADER_STAGE_CALLABLE_BIT_KHR";
    }

    return "";
}

void collectTypes(SpvReflectTypeDescription*** typeCollection, SpvReflectTypeDescription* root)
{
    stbds_arrput((*typeCollection), root);
    for (uint32_t i = 0; i < root->member_count; i++)
    {
        collectTypes(typeCollection, &root->members[i]);
    }
}

static char const* scalarTypeToText(SpvReflectTypeDescription* type)
{
    char const* scalarType = "Error";
    if (type->type_flags & SPV_REFLECT_TYPE_FLAG_INT)
    {
        if (type->traits.numeric.scalar.signedness)
        {
            scalarType = "int32_t";
        }
        else
        {
            scalarType = "uint32_t";
        }
    }
    else if (type->type_flags & SPV_REFLECT_TYPE_FLAG_FLOAT)
    {
        scalarType = "float";
    }
    return scalarType;
}

char const* sanitizeTypeName(char const* name)
{
    for (int32_t i = strlen(name) - 1; i >= 0; i--)
    {
        if (name[i] == '.')
        {
            return name + i + 1;
        }
    }
    return name;
}

void printType(FILE *file, SpvReflectTypeDescription* sourceType)
{
    if (sourceType->type_flags & SPV_REFLECT_TYPE_FLAG_STRUCT)
    {
        fprintf(file, "typedef struct sg_%s\n", sanitizeTypeName(sourceType->type_name));
        fprintf(file, "{\n");
        for (uint32_t i = 0; i < sourceType->member_count; i++)
        {
            SpvReflectTypeDescription* member = &sourceType->members[i];

            if (member->type_flags & SPV_REFLECT_TYPE_FLAG_BOOL)
            {
                fprintf(file, "\tbool %s;\n", member->struct_member_name);
            }
            else if (member->type_flags & SPV_REFLECT_TYPE_FLAG_MATRIX)
            {
                char const* scalarType = scalarTypeToText(member);
                fprintf(file, "\t%s %s[%u][%u];\n", scalarType, member->struct_member_name, member->traits.numeric.matrix.row_count, member->traits.numeric.matrix.column_count);
            }
            else if (member->type_flags & SPV_REFLECT_TYPE_FLAG_VECTOR)
            {
                char const* scalarType = scalarTypeToText(member);
                fprintf(file, "\t%s %s[%u];\n", scalarType, member->struct_member_name, member->traits.numeric.vector.component_count);
            }
            else if (member->type_flags & SPV_REFLECT_TYPE_FLAG_INT)
            {
                char const* scalarType;
                if (member->traits.numeric.scalar.signedness)
                {
                    scalarType = "int";
                }
                else
                {
                    scalarType = "uint";
                }
                fprintf(file, "\t%s%u_t %s;\n", scalarType, member->traits.numeric.scalar.width, member->struct_member_name);
            }
            else if (member->type_flags & SPV_REFLECT_TYPE_FLAG_FLOAT)
            {
                fprintf(file, "\tfloat %s;\n", member->struct_member_name);
            }
            else if (member->type_flags & SPV_REFLECT_TYPE_FLAG_STRUCT)
            {
                fprintf(file, "\tsg_%s %s;\n", sanitizeTypeName(member->type_name), member->struct_member_name);
            }
            else
            {
                assert(false);
            }
        }
        fprintf(file, "} sg_%s;\n", sanitizeTypeName(sourceType->type_name));
        fprintf(file, "\n");
    }
}

int main(int argc, char* argv[])
{
    sargs_setup(&(sargs_desc) {
        .argc = argc,
        .argv = argv
    });

    if (!sargs_exists("-i")
        || !sargs_exists("-o")
        || !sargs_exists("-n"))
    {
        printf("One of our arguments is missing.");
        return 1;
    }

    const char* inputPath = sargs_value("-i");
    const char* outputPath = sargs_value("-o");
    const char* pipelineName = sargs_value("-n");

    char* spirvFile = NULL;
    size_t spirvFileSize;
    readWholeFile(inputPath, &spirvFile, &spirvFileSize);

    SpvReflectShaderModule module;
    SpvReflectResult result = spvReflectCreateShaderModule(spirvFileSize, spirvFile, &module);
    assert(result == SPV_REFLECT_RESULT_SUCCESS);

    uint32_t descriptorBindingCount = 0;
    result = spvReflectEnumerateDescriptorBindings(&module, &descriptorBindingCount, NULL);
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    SpvReflectDescriptorBinding** descriptorBindings =
        (SpvReflectDescriptorBinding**)malloc(descriptorBindingCount * sizeof(SpvReflectDescriptorBinding*));
    result = spvReflectEnumerateDescriptorBindings(&module, &descriptorBindingCount, descriptorBindings);
    assert(result == SPV_REFLECT_RESULT_SUCCESS);

    FILE *outputHeader = fopen(outputPath, "w");
    fprintf(outputHeader, "// Generated header file for %s.\n", pipelineName);

    fprintf(outputHeader, "#include <iceberg/ib_core.h>\n");
    fprintf(outputHeader, "#include <stdint.h>\n");
    fprintf(outputHeader, "\n");

    SpvReflectTypeDescription** typesToGenerate = NULL;

    // Print input bindings
    {
        fprintf(outputHeader, "typedef enum sg_%sInputEnum\n", pipelineName);
        fprintf(outputHeader, "{\n");
        for (uint32_t i = 0; i < descriptorBindingCount; i++)
        {
            fprintf(outputHeader, "\t%s_%s = %u,\n", pipelineName, descriptorBindings[i]->name, i);
        }
        fprintf(outputHeader, "} sg_%sInputEnum;\n", pipelineName);

        fprintf(outputHeader, "\n");

        fprintf(outputHeader, "static ib_ShaderInputDesc const sg_%sInputs[] = \n", pipelineName);
        fprintf(outputHeader, "{\n");
        for (uint32_t i = 0; i < descriptorBindingCount; i++)
        {
            fprintf(outputHeader, "\t{ .Shaders = %s, .Type = %s, .UseImmutableSamplers = %s },\n",
                shaderModuleToString(module.shader_stage),
                descriptorTypeToString(descriptorBindings[i]->descriptor_type),
                descriptorBindings[i]->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER ? "true" : "false");

            if (descriptorBindings[i]->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                descriptorBindings[i]->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            {
                collectTypes(&typesToGenerate, descriptorBindings[i]->type_description);
            }
        }
        fprintf(outputHeader, "};\n");
    }

    fprintf(outputHeader, "\n");

    for (int32_t i = stbds_arrlen(typesToGenerate) - 1; i >= 0; i--)
    {
        printType(outputHeader, typesToGenerate[i]);
    }
    stbds_arrfree(typesToGenerate);

    if (module.spirv_execution_model == SpvExecutionModelGLCompute)
    {
        fprintf(outputHeader, "const struct { uint32_t x, y, z; } sg_%sThreadGroupSize = { %u, %u, %u };\n",
            pipelineName,
            module.entry_points[0].local_size.x, module.entry_points[0].local_size.y, module.entry_points[0].local_size.z);
    }

    fclose(outputHeader);

    free(descriptorBindings);
    spvReflectDestroyShaderModule(&module);

    return 0;
}