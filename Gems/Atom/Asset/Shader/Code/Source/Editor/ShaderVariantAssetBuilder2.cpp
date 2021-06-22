/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*/

#include <ShaderVariantAssetBuilder2.h>

#include <Atom/RPI.Reflect/Shader/ShaderAsset2.h>
#include <Atom/RPI.Reflect/Shader/ShaderVariantAsset2.h>
#include <Atom/RPI.Reflect/Shader/ShaderVariantTreeAsset.h>
#include <Atom/RPI.Reflect/Shader/ShaderOptionGroup.h>

#include <Atom/RPI.Edit/Shader/ShaderVariantListSourceData.h>
#include <Atom/RPI.Edit/Shader/ShaderVariantAssetCreator2.h>
#include <Atom/RPI.Edit/Shader/ShaderVariantTreeAssetCreator.h>
#include <Atom/RPI.Edit/Common/JsonUtils.h>

#include <AtomCore/Serialization/Json/JsonUtils.h>
#include <Atom/RPI.Reflect/Shader/ShaderResourceGroupAsset.h>
#include <Atom/RPI.Reflect/Shader/ShaderVariantKey.h>

#include <Atom/RHI.Edit/Utils.h>
#include <Atom/RHI.Edit/ShaderPlatformInterface.h>
#include <Atom/RPI.Edit/Common/JsonReportingHelper.h>
#include <Atom/RPI.Edit/Common/AssetUtils.h>
#include <Atom/RHI.Reflect/ConstantsLayout.h>
#include <Atom/RHI.Reflect/PipelineLayoutDescriptor.h>
#include <Atom/RHI.Reflect/ShaderStageFunction.h>

#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/Debug/TraceContext.h>

#include <AzFramework/API/ApplicationAPI.h>
#include <AzFramework/StringFunc/StringFunc.h>
#include <AzFramework/IO/LocalFileIO.h>
#include <AzFramework/Platform/PlatformDefaults.h>

#include <AzCore/Asset/AssetManager.h>
#include <AzCore/JSON/document.h>
#include <AzCore/IO/FileIO.h>
#include <AzCore/IO/IOUtils.h>
#include <AzCore/IO/SystemFile.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/sort.h>
#include <AzCore/Serialization/Json/JsonSerialization.h>

#include "ShaderAssetBuilder2.h"
#include "ShaderBuilderUtility.h"
#include "AzslData.h"
#include "AzslCompiler.h"
#include "AzslBuilder.h"
#include <CommonFiles/Preprocessor.h>
#include <CommonFiles/GlobalBuildOptions.h>
#include <ShaderPlatformInterfaceRequest.h>
#include "AtomShaderConfig.h"

namespace AZ
{
    namespace ShaderBuilder
    {
        static constexpr char ShaderVariantAssetBuilder2Name[] = "ShaderVariantAssetBuilder2";

        static void AddShaderAssetJobDependency2(
            AssetBuilderSDK::JobDescriptor& jobDescriptor, const AssetBuilderSDK::PlatformInfo& platformInfo,
            const AZStd::string& shaderVariantListFilePath, const AZStd::string& shaderFilePath)
        {
            AZStd::vector<AZStd::string> possibleDependencies =
                AZ::RPI::AssetUtils::GetPossibleDepenencyPaths(shaderVariantListFilePath, shaderFilePath);
            for (auto& file : possibleDependencies)
            {
                AssetBuilderSDK::JobDependency jobDependency;
                jobDependency.m_jobKey = ShaderAssetBuilder2::ShaderAssetBuilder2JobKey;
                jobDependency.m_platformIdentifier = platformInfo.m_identifier;
                jobDependency.m_type = AssetBuilderSDK::JobDependencyType::Order;
                jobDependency.m_sourceFile.m_sourceFileDependencyPath = file;
                jobDescriptor.m_jobDependencyList.push_back(jobDependency);
            }
        }

        //! Returns true if @sourceFileFullPath starts with a valid asset processor scan folder, false otherwise.
        //! In case of true, it splits @sourceFileFullPath into @scanFolderFullPath and @filePathFromScanFolder.
        //! @sourceFileFullPath The full path to a source asset file.
        //! @scanFolderFullPath [out] Gets the full path of the scan folder where the source file is located.
        //! @filePathFromScanFolder [out] Get the file path relative to  @scanFolderFullPath.
        static bool SplitSourceAssetPathIntoScanFolderFullPathAndRelativeFilePath2(const AZStd::string& sourceFileFullPath, AZStd::string& scanFolderFullPath, AZStd::string& filePathFromScanFolder)
        {
            AZStd::vector<AZStd::string> scanFolders;
            bool success = false;
            AzToolsFramework::AssetSystemRequestBus::BroadcastResult(success, &AzToolsFramework::AssetSystem::AssetSystemRequest::GetAssetSafeFolders, scanFolders);
            if (!success)
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "Couldn't get the scan folders");
                return false;
            }

            for (AZStd::string scanFolder : scanFolders)
            {
                AzFramework::StringFunc::Path::Normalize(scanFolder);
                if (!AZ::StringFunc::StartsWith(sourceFileFullPath, scanFolder))
                {
                    continue;
                }
                const size_t scanFolderSize = scanFolder.size();
                const size_t sourcePathSize = sourceFileFullPath.size();
                scanFolderFullPath = scanFolder;
                filePathFromScanFolder = sourceFileFullPath.substr(scanFolderSize + 1, sourcePathSize - scanFolderSize - 1);
                return true;
            }

            return false;
        }

        //! Validates if a given .shadervariantlist file is located at the correct path for a given .shader full path.
        //! There are two valid paths:
        //! 1- Lower Precedence: The same folder where the .shader file is located.
        //! 2- Higher Precedence: <DEVROOT>/<GAME>/ShaderVariants/<Same Scan Folder Subpath as the .shader file>.
        //! The "Higher Precedence" path gives the option to game projects to override what variants to generate. If this
        //!     file exists then the "Lower Precedence" path is disregarded.
        //! A .shader full path is located under an AP scan folder.
        //! Example: "<DEVROOT>/Gems/Atom/Feature/Common/Assets/Materials/Types/StandardPBR_ForwardPass.shader"
        //!     - In this example the Scan Folder is "<DEVROOT>/Gems/Atom/Feature/Common/Assets", while the subfolder is "Materials/Types".
        //! The "Higher Precedence" expected valid location for the .shadervariantlist would be:
        //!     - <DEVROOT>/<GameProject>/ShaderVariants/Materials/Types/StandardPBR_ForwardPass.shadervariantlist.
        //! The "Lower Precedence" valid location would be:
        //!     - <DEVROOT>/Gems/Atom/Feature/Common/Assets/Materials/Types/StandardPBR_ForwardPass.shadervariantlist.
        //! @shouldExitEarlyFromProcessJob [out] Set to true if ProcessJob should do no work but return successfully.
        //!     Set to false if ProcessJob should do work and create assets.
        //!     When @shaderVariantListFileFullPath is provided by a Gem/Feature instead of the Game Project
        //!     We check if the game project already defined the shader variant list, and if it did it means
        //!     ProcessJob should do no work, but return successfully nonetheless.
        static bool ValidateShaderVariantListLocation2(const AZStd::string& shaderVariantListFileFullPath,
            const AZStd::string& shaderFileFullPath, bool& shouldExitEarlyFromProcessJob)
        {
            AZStd::string scanFolderFullPath;
            AZStd::string shaderProductFileRelativePath;
            if (!SplitSourceAssetPathIntoScanFolderFullPathAndRelativeFilePath2(shaderFileFullPath, scanFolderFullPath, shaderProductFileRelativePath))
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "Couldn't get the scan folder for shader [%s]", shaderFileFullPath.c_str());
                return false;
            }
            AZ_TracePrintf(ShaderVariantAssetBuilder2Name, "For shader [%s], Scan folder full path [%s], relative file path [%s]", shaderFileFullPath.c_str(), scanFolderFullPath.c_str(), shaderProductFileRelativePath.c_str());

            AZStd::string shaderVariantListFileRelativePath = shaderProductFileRelativePath;
            AzFramework::StringFunc::Path::ReplaceExtension(shaderVariantListFileRelativePath, RPI::ShaderVariantListSourceData::Extension);

            const char * gameProjectPath = nullptr;
            AzToolsFramework::AssetSystemRequestBus::BroadcastResult(gameProjectPath, &AzToolsFramework::AssetSystem::AssetSystemRequest::GetAbsoluteDevGameFolderPath);

            AZStd::string expectedHigherPrecedenceFileFullPath;
            AzFramework::StringFunc::Path::Join(gameProjectPath, RPI::ShaderVariantTreeAsset::CommonSubFolder, expectedHigherPrecedenceFileFullPath, false /* handle directory overlap? */, false /* be case insensitive? */);
            AzFramework::StringFunc::Path::Join(expectedHigherPrecedenceFileFullPath.c_str(), shaderProductFileRelativePath.c_str(), expectedHigherPrecedenceFileFullPath, false /* handle directory overlap? */, false /* be case insensitive? */);
            AzFramework::StringFunc::Path::ReplaceExtension(expectedHigherPrecedenceFileFullPath, AZ::RPI::ShaderVariantListSourceData::Extension);
            AzFramework::StringFunc::Path::Normalize(expectedHigherPrecedenceFileFullPath);

            AZStd::string normalizedShaderVariantListFileFullPath = shaderVariantListFileFullPath;
            AzFramework::StringFunc::Path::Normalize(normalizedShaderVariantListFileFullPath);

            if (expectedHigherPrecedenceFileFullPath == normalizedShaderVariantListFileFullPath)
            {
                // Whenever the Game Project declares a *.shadervariantlist file we always do work.
                shouldExitEarlyFromProcessJob = false;
                return true;
            }

            AZ::Data::AssetInfo assetInfo;
            AZStd::string watchFolder;
            bool foundHigherPrecedenceAsset = false;
            AzToolsFramework::AssetSystemRequestBus::BroadcastResult(foundHigherPrecedenceAsset
                , &AzToolsFramework::AssetSystem::AssetSystemRequest::GetSourceInfoBySourcePath
                , expectedHigherPrecedenceFileFullPath.c_str(), assetInfo, watchFolder);
            if (foundHigherPrecedenceAsset)
            {
                AZ_TracePrintf(ShaderVariantAssetBuilder2Name, "The shadervariantlist [%s] has been overriden by the game project with [%s]",
                    normalizedShaderVariantListFileFullPath.c_str(), expectedHigherPrecedenceFileFullPath.c_str());
                shouldExitEarlyFromProcessJob = true;
                return true;
            }

            // Check the "Lower Precedence" case, .shader path == .shadervariantlist path.
            AZStd::string normalizedShaderFileFullPath = shaderFileFullPath;
            AzFramework::StringFunc::Path::Normalize(normalizedShaderFileFullPath);

            AZStd::string normalizedShaderFileFullPathWithoutExtension = normalizedShaderFileFullPath;
            AzFramework::StringFunc::Path::StripExtension(normalizedShaderFileFullPathWithoutExtension);

            AZStd::string normalizedShaderVariantListFileFullPathWithoutExtension = normalizedShaderVariantListFileFullPath;
            AzFramework::StringFunc::Path::StripExtension(normalizedShaderVariantListFileFullPathWithoutExtension);

#if AZ_TRAIT_OS_USE_WINDOWS_FILE_PATHS
            //In certain circumstances, the capitalization of the drive letter may not match
            const bool caseSensitive = false;
#else
            //On the other platforms there's no drive letter, so it should be a non-issue.
            const bool caseSensitive = true;
#endif
            if (!StringFunc::Equal(normalizedShaderFileFullPathWithoutExtension.c_str(), normalizedShaderVariantListFileFullPathWithoutExtension.c_str(), caseSensitive))
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "For shader file at path [%s], the shader variant list [%s] is expected to be located at [%s.%s] or [%s]"
                    , normalizedShaderFileFullPath.c_str(), normalizedShaderVariantListFileFullPath.c_str(),
                    normalizedShaderFileFullPathWithoutExtension.c_str(), RPI::ShaderVariantListSourceData::Extension,
                    expectedHigherPrecedenceFileFullPath.c_str());
                return false;
            }

            shouldExitEarlyFromProcessJob = false;
            return true;
        }

        // We treat some issues as warnings and return "Success" from CreateJobs allows us to report the dependency.
        // If/when a valid dependency file appears, that will trigger the ShaderVariantAssetBuilder2 to run again.
        // Since CreateJobs will pass, we forward this message to ProcessJob which will report it as an error.
        struct LoadResult2
        {
            enum class Code
            {
                Error,
                DeferredError,
                Success
            };

            Code m_code;
            AZStd::string m_deferredMessage; // Only used when m_code == DeferredError
        };

        static LoadResult2 LoadShaderVariantList2(const AZStd::string& variantListFullPath, RPI::ShaderVariantListSourceData& shaderVariantList, AZStd::string& shaderSourceFileFullPath,
            bool& shouldExitEarlyFromProcessJob)
        {
            // Need to get the name of the shader file from the template so that we can preprocess the shader data and setup
            // source file dependencies.
            if (!RPI::JsonUtils::LoadObjectFromFile(variantListFullPath, shaderVariantList))
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "Failed to parse Shader Variant List Descriptor JSON from [%s]", variantListFullPath.c_str());
                return LoadResult2{LoadResult2::Code::Error};
            }

            const AZStd::string resolvedShaderPath = AZ::RPI::AssetUtils::ResolvePathReference(variantListFullPath, shaderVariantList.m_shaderFilePath);
            if (!AZ::IO::LocalFileIO::GetInstance()->Exists(resolvedShaderPath.c_str()))
            {
                return LoadResult2{LoadResult2::Code::DeferredError, AZStd::string::format("The shader path [%s] was not found.", resolvedShaderPath.c_str())};
            }

            shaderSourceFileFullPath = resolvedShaderPath;

            if (!ValidateShaderVariantListLocation2(variantListFullPath, shaderSourceFileFullPath, shouldExitEarlyFromProcessJob))
            {
                return LoadResult2{LoadResult2::Code::Error};
            }

            if (shouldExitEarlyFromProcessJob)
            {
                return LoadResult2{LoadResult2::Code::Success};
            }

            auto resultOutcome = RPI::ShaderVariantTreeAssetCreator::ValidateStableIdsAreUnique(shaderVariantList.m_shaderVariants);
            if (!resultOutcome.IsSuccess())
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "Variant info validation error: %s", resultOutcome.GetError().c_str());
                return LoadResult2{LoadResult2::Code::Error};
            }

            if (!IO::FileIOBase::GetInstance()->Exists(shaderSourceFileFullPath.c_str()))
            {
                return LoadResult2{LoadResult2::Code::DeferredError, AZStd::string::format("ShaderSourceData file does not exist: %s.", shaderSourceFileFullPath.c_str())};
            }

            return LoadResult2{LoadResult2::Code::Success};
        } // LoadShaderVariantListAndAzslSource

        void ShaderVariantAssetBuilder2::CreateJobs(const AssetBuilderSDK::CreateJobsRequest& request, AssetBuilderSDK::CreateJobsResponse& response) const
        {
            AZStd::string variantListFullPath;
            AzFramework::StringFunc::Path::ConstructFull(request.m_watchFolder.data(), request.m_sourceFile.data(), variantListFullPath, true);

            AZ_TracePrintf(ShaderVariantAssetBuilder2Name, "CreateJobs for Shader Variant List \"%s\"\n", variantListFullPath.data());

            RPI::ShaderVariantListSourceData shaderVariantList;
            AZStd::string shaderSourceFileFullPath;
            bool shouldExitEarlyFromProcessJob = false;
            const LoadResult2 loadResult = LoadShaderVariantList2(variantListFullPath, shaderVariantList, shaderSourceFileFullPath, shouldExitEarlyFromProcessJob);

            if (loadResult.m_code == LoadResult2::Code::Error)
            {
                response.m_result = AssetBuilderSDK::CreateJobsResultCode::Failed;
                return;
            }

            if (loadResult.m_code == LoadResult2::Code::DeferredError || shouldExitEarlyFromProcessJob)
            {
                for (const AssetBuilderSDK::PlatformInfo& info : request.m_enabledPlatforms)
                {
                    // Let's create fake jobs that will fail ProcessJob, but are useful to establish dependency on the shader file.
                    AssetBuilderSDK::JobDescriptor jobDescriptor;

                    jobDescriptor.m_priority = -5000;
                    jobDescriptor.m_critical = false;
                    jobDescriptor.m_jobKey = ShaderVariantAssetBuilder2JobKey;
                    jobDescriptor.SetPlatformIdentifier(info.m_identifier.data());

                    AddShaderAssetJobDependency2(jobDescriptor, info, variantListFullPath, shaderVariantList.m_shaderFilePath);

                    if (loadResult.m_code == LoadResult2::Code::DeferredError)
                    {
                        jobDescriptor.m_jobParameters.emplace(ShaderVariantLoadErrorParam, loadResult.m_deferredMessage);
                    }

                    if (shouldExitEarlyFromProcessJob)
                    {
                        // The value doesn't matter, what matters is the presence of the key which will
                        // signal that no assets should be produced on behalf of this shadervariantlist because
                        // the game project overrode it.
                        jobDescriptor.m_jobParameters.emplace(ShouldExitEarlyFromProcessJobParam, variantListFullPath);
                    }

                    response.m_createJobOutputs.push_back(jobDescriptor);
                }
                response.m_result = AssetBuilderSDK::CreateJobsResultCode::Success;
                return;
            }

            for (const AssetBuilderSDK::PlatformInfo& info : request.m_enabledPlatforms)
            {
                AZ_TraceContext("For platform", info.m_identifier.data());

                // First job is for the ShaderVariantTreeAsset.
                {
                    AssetBuilderSDK::JobDescriptor jobDescriptor;
                
                    // The ShaderVariantTreeAsset is high priority, but must be generated after the ShaderAsset 
                    jobDescriptor.m_priority = 1;
                    jobDescriptor.m_critical = false;
                
                    jobDescriptor.m_jobKey = GetShaderVariantTreeAssetJobKey();
                    jobDescriptor.SetPlatformIdentifier(info.m_identifier.data());
                
                    AddShaderAssetJobDependency2(jobDescriptor, info, variantListFullPath, shaderVariantList.m_shaderFilePath);
                
                    jobDescriptor.m_jobParameters.emplace(ShaderSourceFilePathJobParam, shaderSourceFileFullPath);
                
                    response.m_createJobOutputs.push_back(jobDescriptor);
                }

                // One job for each variant. Each job will produce one ".azshadervariant" per RHI per supervariant.
                for (const AZ::RPI::ShaderVariantListSourceData::VariantInfo& variantInfo : shaderVariantList.m_shaderVariants)
                {
                    AZStd::string variantInfoAsJsonString;
                    const bool convertSuccess = AZ::RPI::JsonUtils::SaveObjectToJsonString(variantInfo, variantInfoAsJsonString);
                    AZ_Assert(convertSuccess, "Failed to convert VariantInfo to json string");

                    AssetBuilderSDK::JobDescriptor jobDescriptor;

                    // There can be tens/hundreds of thousands of shader variants. By default each shader will get
                    // a root variant that can be used at runtime. In order to prevent the AssetProcessor from
                    // being overtaken by shader variant compilation We mark all non-root shader variant generation
                    // as non critical and very low priority.
                    jobDescriptor.m_priority = -5000;
                    jobDescriptor.m_critical = false;

                    jobDescriptor.m_jobKey = GetShaderVariantAssetJobKey(RPI::ShaderVariantStableId{variantInfo.m_stableId});
                    jobDescriptor.SetPlatformIdentifier(info.m_identifier.data());

                    // The ShaderVariantAssets are job dependent on the ShaderVariantTreeAsset.
                    AssetBuilderSDK::SourceFileDependency fileDependency;
                    fileDependency.m_sourceFileDependencyPath = variantListFullPath;
                    AssetBuilderSDK::JobDependency variantTreeJobDependency;
                    variantTreeJobDependency.m_jobKey = GetShaderVariantTreeAssetJobKey();
                    variantTreeJobDependency.m_platformIdentifier = info.m_identifier;
                    variantTreeJobDependency.m_sourceFile = fileDependency;
                    variantTreeJobDependency.m_type = AssetBuilderSDK::JobDependencyType::Order;
                    jobDescriptor.m_jobDependencyList.emplace_back(variantTreeJobDependency);

                    jobDescriptor.m_jobParameters.emplace(ShaderVariantJobVariantParam, variantInfoAsJsonString);
                    jobDescriptor.m_jobParameters.emplace(ShaderSourceFilePathJobParam, shaderSourceFileFullPath);

                    response.m_createJobOutputs.push_back(jobDescriptor);
                }

            }
            response.m_result = AssetBuilderSDK::CreateJobsResultCode::Success;
        }  // CreateJobs

        void ShaderVariantAssetBuilder2::ProcessJob(const AssetBuilderSDK::ProcessJobRequest& request, AssetBuilderSDK::ProcessJobResponse& response) const
        {
            const auto& jobParameters = request.m_jobDescription.m_jobParameters;

            if (jobParameters.find(ShaderVariantLoadErrorParam) != jobParameters.end())
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "Error during CreateJobs: %s", jobParameters.at(ShaderVariantLoadErrorParam).c_str());
                response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
                return;
            }
            
            if (jobParameters.find(ShouldExitEarlyFromProcessJobParam) != jobParameters.end())
            {
                AZ_TracePrintf(ShaderVariantAssetBuilder2Name, "Doing nothing on behalf of [%s] because it's been overridden by game project.", jobParameters.at(ShaderVariantLoadErrorParam).c_str());
                response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Success;
                return;
            }

            AssetBuilderSDK::JobCancelListener jobCancelListener(request.m_jobId);
            if (jobCancelListener.IsCancelled())
            {
                response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Cancelled;
                return;
            }

            if (request.m_jobDescription.m_jobKey == GetShaderVariantTreeAssetJobKey())
            {
                ProcessShaderVariantTreeJob(request, response);
            }
            else
            {
                ProcessShaderVariantJob(request, response);
            }
        }


        static RPI::Ptr<RPI::ShaderOptionGroupLayout> LoadShaderOptionsGroupLayoutFromShaderAssetBuilder2(
            const RHI::ShaderPlatformInterface* shaderPlatformInterface,
            const AssetBuilderSDK::PlatformInfo& platformInfo,
            const AzslCompiler& azslCompiler,
            const AZStd::string& shaderSourceFileFullPath,
            const RPI::SupervariantIndex supervariantIndex)
        {
            auto optionsGroupPathOutcome = ShaderBuilderUtility::ObtainBuildArtifactPathFromShaderAssetBuilder2(
                shaderPlatformInterface->GetAPIUniqueIndex(), platformInfo.m_identifier, shaderSourceFileFullPath, supervariantIndex.GetIndex(),
                AZ::RPI::ShaderAssetSubId::OptionsJson);
            if (!optionsGroupPathOutcome.IsSuccess())
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "%s", optionsGroupPathOutcome.GetError().c_str());
                return nullptr;
            }
            auto optionsGroupJsonPath = optionsGroupPathOutcome.TakeValue();
            RPI::Ptr<RPI::ShaderOptionGroupLayout> shaderOptionGroupLayout = RPI::ShaderOptionGroupLayout::Create();
            // The shader options define what options are available, what are the allowed values/range
            // for each option and what is its default value.
            auto jsonOutcome = JsonSerializationUtils::ReadJsonFile(optionsGroupJsonPath);
            if (!jsonOutcome.IsSuccess())
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "%s", jsonOutcome.GetError().c_str());
                return nullptr;
            }
            if (!azslCompiler.ParseOptionsPopulateOptionGroupLayout(jsonOutcome.GetValue(), shaderOptionGroupLayout))
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "Failed to find a valid list of shader options!");
                return nullptr;
            }

            return shaderOptionGroupLayout;
        }

        static void LoadShaderFunctionsFromShaderAssetBuilder2(
            const RHI::ShaderPlatformInterface* shaderPlatformInterface, const AssetBuilderSDK::PlatformInfo& platformInfo,
            const AzslCompiler& azslCompiler, const AZStd::string& shaderSourceFileFullPath,
            const RPI::SupervariantIndex supervariantIndex,
            AzslFunctions& functions)
        {
            auto functionsJsonPathOutcome = ShaderBuilderUtility::ObtainBuildArtifactPathFromShaderAssetBuilder2(
                shaderPlatformInterface->GetAPIUniqueIndex(), platformInfo.m_identifier, shaderSourceFileFullPath, supervariantIndex.GetIndex(),
                AZ::RPI::ShaderAssetSubId::IaJson);
            if (!functionsJsonPathOutcome.IsSuccess())
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "%s", functionsJsonPathOutcome.GetError().c_str());
                return;
            }

            auto functionsJsonPath = functionsJsonPathOutcome.TakeValue();
            auto jsonOutcome = JsonSerializationUtils::ReadJsonFile(functionsJsonPath);
            if (!jsonOutcome.IsSuccess())
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "%s", jsonOutcome.GetError().c_str());
                return;
            }
            if (!azslCompiler.ParseIaPopulateFunctionData(jsonOutcome.GetValue(), functions))
            {
                functions.clear();
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "Failed to find shader functions.");
                return;
            }
        }


        // Returns the content of the hlsl file for the given supervariant as produced by ShaderAsssetBuilder2.
        // In addition to the content it also returns the full path of the hlsl file in @hlslSourcePath.
        static AZStd::string LoadHlslFileFromShaderAssetBuilder2(
            const RHI::ShaderPlatformInterface* shaderPlatformInterface, const AssetBuilderSDK::PlatformInfo& platformInfo,
            const AZStd::string& shaderSourceFileFullPath, const RPI::SupervariantIndex supervariantIndex, AZStd::string& hlslSourcePath)
        {
            auto hlslSourcePathOutcome = ShaderBuilderUtility::ObtainBuildArtifactPathFromShaderAssetBuilder2(
                shaderPlatformInterface->GetAPIUniqueIndex(), platformInfo.m_identifier, shaderSourceFileFullPath, supervariantIndex.GetIndex(),
                AZ::RPI::ShaderAssetSubId::GeneratedHlslSource);
            if (!hlslSourcePathOutcome.IsSuccess())
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "%s", hlslSourcePathOutcome.GetError().c_str());
                return "";
            }

            hlslSourcePath = hlslSourcePathOutcome.TakeValue();
            Outcome<AZStd::string, AZStd::string> hlslSourceOutcome = Utils::ReadFile(hlslSourcePath);
            if (!hlslSourceOutcome.IsSuccess())
            {
                AZ_Error(
                    ShaderVariantAssetBuilder2Name, false, "Failed to obtain shader source from %s. [%s]", hlslSourcePath.c_str(),
                    hlslSourceOutcome.TakeError().c_str());
                return "";
            }
            return hlslSourceOutcome.TakeValue();
        }

        void ShaderVariantAssetBuilder2::ProcessShaderVariantTreeJob(const AssetBuilderSDK::ProcessJobRequest& request, AssetBuilderSDK::ProcessJobResponse& response) const
        {
            AZStd::string variantListFullPath;
            AzFramework::StringFunc::Path::ConstructFull(request.m_watchFolder.data(), request.m_sourceFile.data(), variantListFullPath, true);

            RPI::ShaderVariantListSourceData shaderVariantListDescriptor;
            if (!RPI::JsonUtils::LoadObjectFromFile(variantListFullPath, shaderVariantListDescriptor))
            {
                AZ_Assert(false, "Failed to parse Shader Variant List Descriptor JSON [%s]", variantListFullPath.c_str());
                response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
                return;
            }

            const AZStd::string& shaderSourceFileFullPath = request.m_jobDescription.m_jobParameters.at(ShaderSourceFilePathJobParam);

            //For debugging purposes will create a dummy azshadervarianttree file.
            AZStd::string shaderName;
            AzFramework::StringFunc::Path::GetFileName(shaderSourceFileFullPath.c_str(), shaderName);

            // No error checking because the same calls were already executed during CreateJobs()
            auto descriptorParseOutcome = ShaderBuilderUtility::LoadShaderDataJson(shaderSourceFileFullPath);
            RPI::ShaderSourceData shaderSourceDescriptor = descriptorParseOutcome.TakeValue();
            RPI::Ptr<RPI::ShaderOptionGroupLayout> shaderOptionGroupLayout;

            // Request the list of valid shader platform interfaces for the target platform.
            AZStd::vector<RHI::ShaderPlatformInterface*> platformInterfaces =
                ShaderBuilderUtility::DiscoverEnabledShaderPlatformInterfaces(request.m_platformInfo, shaderSourceDescriptor);
            if (platformInterfaces.empty())
            {
                // No work to do. Exit gracefully.
                AZ_TracePrintf(
                    ShaderVariantAssetBuilder2Name,
                    "No azshadervarianttree is produced on behalf of %s because all valid RHI backends were disabled for this shader.\n",
                    shaderSourceFileFullPath.c_str());
                response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Success;
                return;
            }


            // set the input file for eventual error messages, but the compiler won't be called on it.
            AZStd::string azslFullPath;
            ShaderBuilderUtility::GetAbsolutePathToAzslFile(shaderSourceFileFullPath, shaderSourceDescriptor.m_source, azslFullPath);
            AzslCompiler azslc(azslFullPath);

            AZStd::string previousLoopApiName;
            for (RHI::ShaderPlatformInterface* shaderPlatformInterface : platformInterfaces)
            {
                auto thisLoopApiName = shaderPlatformInterface->GetAPIName().GetStringView();
                RPI::Ptr<RPI::ShaderOptionGroupLayout> loopLocal_ShaderOptionGroupLayout =
                    LoadShaderOptionsGroupLayoutFromShaderAssetBuilder2(
                        shaderPlatformInterface, request.m_platformInfo, azslc, shaderSourceFileFullPath, RPI::DefaultSupervariantIndex);
                if (!loopLocal_ShaderOptionGroupLayout)
                {
                    response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
                    return;
                }
                if (shaderOptionGroupLayout && shaderOptionGroupLayout->GetHash() != loopLocal_ShaderOptionGroupLayout->GetHash())
                {
                    AZ_Error(ShaderVariantAssetBuilder2Name, false, "There was a discrepancy in shader options between %s and %s", previousLoopApiName.c_str(), thisLoopApiName.data());
                    response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
                    return;
                }
                shaderOptionGroupLayout = loopLocal_ShaderOptionGroupLayout;
                previousLoopApiName = thisLoopApiName;
            }

            RPI::ShaderVariantTreeAssetCreator shaderVariantTreeAssetCreator;
            shaderVariantTreeAssetCreator.Begin(Uuid::CreateRandom());
            shaderVariantTreeAssetCreator.SetShaderOptionGroupLayout(*shaderOptionGroupLayout);
            shaderVariantTreeAssetCreator.SetVariantInfos(shaderVariantListDescriptor.m_shaderVariants);
            Data::Asset<RPI::ShaderVariantTreeAsset> shaderVariantTreeAsset;
            if (!shaderVariantTreeAssetCreator.End(shaderVariantTreeAsset))
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "Failed to build Shader Variant Tree Asset");
                response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
                return;
            }

            AZStd::string filename = AZStd::string::format("%s.%s", shaderName.c_str(), RPI::ShaderVariantTreeAsset::Extension);
            AZStd::string assetPath;
            AzFramework::StringFunc::Path::ConstructFull(request.m_tempDirPath.c_str(), filename.c_str(), assetPath, true);
            if (!AZ::Utils::SaveObjectToFile(assetPath, AZ::DataStream::ST_BINARY, shaderVariantTreeAsset.Get()))
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "Failed to save Shader Variant Tree Asset to \"%s\"", assetPath.c_str());
                response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
                return;
            }

            AssetBuilderSDK::JobProduct assetProduct;
            assetProduct.m_productSubID = RPI::ShaderVariantTreeAsset::ProductSubID;
            assetProduct.m_productFileName = assetPath;
            assetProduct.m_productAssetType = azrtti_typeid<RPI::ShaderVariantTreeAsset>();
            assetProduct.m_dependenciesHandled = true; // This builder has no dependencies to output
            response.m_outputProducts.push_back(assetProduct);

            AZ_TracePrintf(ShaderVariantAssetBuilder2Name, "Shader Variant Tree Asset [%s] compiled successfully.\n", assetPath.c_str());

            response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Success;
        }

        void ShaderVariantAssetBuilder2::ProcessShaderVariantJob(const AssetBuilderSDK::ProcessJobRequest& request, AssetBuilderSDK::ProcessJobResponse& response) const
        {
            const AZStd::sys_time_t startTime = AZStd::GetTimeNowTicks();
            AssetBuilderSDK::JobCancelListener jobCancelListener(request.m_jobId);

            AZStd::string fullPath;
            AzFramework::StringFunc::Path::ConstructFull(request.m_watchFolder.data(), request.m_sourceFile.data(), fullPath, true);

            const auto& jobParameters = request.m_jobDescription.m_jobParameters;
            const AZStd::string& shaderSourceFileFullPath = jobParameters.at(ShaderSourceFilePathJobParam);
            AZStd::string shaderFileName;
            AzFramework::StringFunc::Path::GetFileName(shaderSourceFileFullPath.c_str(), shaderFileName);

            const AZStd::string& variantJsonString = jobParameters.at(ShaderVariantJobVariantParam);
            RPI::ShaderVariantListSourceData::VariantInfo variantInfo;
            const bool fromJsonStringSuccess = AZ::RPI::JsonUtils::LoadObjectFromJsonString(variantJsonString, variantInfo);
            AZ_Assert(fromJsonStringSuccess, "Failed to convert json string to VariantInfo");

            RPI::ShaderSourceData shaderSourceDescriptor;
            AZStd::shared_ptr<ShaderFiles> sources = ShaderBuilderUtility::PrepareSourceInput(ShaderVariantAssetBuilder2Name, shaderSourceFileFullPath, shaderSourceDescriptor);

            // set the input file for eventual error messages, but the compiler won't be called on it.
            AzslCompiler azslc(sources->m_azslSourceFullPath);

            // Request the list of valid shader platform interfaces for the target platform.
            AZStd::vector<RHI::ShaderPlatformInterface*> platformInterfaces =
                ShaderBuilderUtility::DiscoverEnabledShaderPlatformInterfaces(request.m_platformInfo, shaderSourceDescriptor);
            if (platformInterfaces.empty())
            {
                // No work to do. Exit gracefully.
                AZ_TracePrintf(ShaderVariantAssetBuilder2Name,
                    "No azshader is produced on behalf of %s because all valid RHI backends were disabled for this shader.\n",
                    shaderSourceFileFullPath.c_str());
                response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Success;
                return;
            }

            auto supervariantList = ShaderBuilderUtility::GetSupervariantListFromShaderSourceData(shaderSourceDescriptor);

            GlobalBuildOptions buildOptions = ReadBuildOptions(ShaderVariantAssetBuilder2Name);
            // At this moment We have global build options that should be merged with the build options that are common
            // to all the supervariants of this shader.
            buildOptions.m_compilerArguments.Merge(shaderSourceDescriptor.m_compiler);

            //! The ShaderOptionGroupLayout is common across all RHIs & Supervariants
            RPI::Ptr<RPI::ShaderOptionGroupLayout> shaderOptionGroupLayout = nullptr;

            // Generate shaders for each of those ShaderPlatformInterfaces.
            for (RHI::ShaderPlatformInterface* shaderPlatformInterface : platformInterfaces)
            {
                AZ_TraceContext("ShaderPlatformInterface", shaderPlatformInterface->GetAPIName().GetCStr());

                // Loop through all the Supervariants.
                uint32_t supervariantIndexCounter = 0;
                for (const auto& supervariantInfo : supervariantList)
                {
                    RPI::SupervariantIndex supervariantIndex(supervariantIndexCounter);

                    // Check if we were canceled before we do any heavy processing of
                    // the shader variant data.
                    if (jobCancelListener.IsCancelled())
                    {
                        response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Cancelled;
                        return;
                    }

                    AZStd::string shaderStemNamePrefix = shaderFileName;
                    if (supervariantIndex.GetIndex() > 0)
                    {
                        shaderStemNamePrefix += supervariantInfo.m_name.GetStringView();
                    }

                    // We need these additional pieces of information To build a shader variant asset:
                    // 1- ShaderOptionsGroupLayout (Need to load it once, because it's the same acrosss all supervariants +  RHIs)
                    // 2- entryFunctions
                    // 3- hlsl code.

                    // 1- ShaderOptionsGroupLayout
                    if (!shaderOptionGroupLayout)
                    {
                        shaderOptionGroupLayout =
                            LoadShaderOptionsGroupLayoutFromShaderAssetBuilder2(
                                shaderPlatformInterface, request.m_platformInfo, azslc, shaderSourceFileFullPath, supervariantIndex);
                        if (!shaderOptionGroupLayout)
                        {
                            response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
                            return;
                        }
                    }

                    // 2- entryFunctions.
                    AzslFunctions azslFunctions;
                    LoadShaderFunctionsFromShaderAssetBuilder2(
                        shaderPlatformInterface, request.m_platformInfo, azslc, shaderSourceFileFullPath, supervariantIndex,  azslFunctions);
                    if (azslFunctions.empty())
                    {
                        response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
                        return;
                    }
                    MapOfStringToStageType shaderEntryPoints;
                    if (shaderSourceDescriptor.m_programSettings.m_entryPoints.empty())
                    {
                        AZ_TracePrintf(
                            ShaderVariantAssetBuilder2Name,
                            "ProgramSettings do not specify entry points, will use GetDefaultEntryPointsFromShader()\n");
                        ShaderBuilderUtility::GetDefaultEntryPointsFromFunctionDataList(azslFunctions, shaderEntryPoints);
                    }
                    else
                    {
                        for (const auto& entryPoint : shaderSourceDescriptor.m_programSettings.m_entryPoints)
                        {
                            shaderEntryPoints[entryPoint.m_name] = entryPoint.m_type;
                        }
                    }

                    // 3- hlslCode
                    AZStd::string hlslSourcePath;
                    AZStd::string hlslCode = LoadHlslFileFromShaderAssetBuilder2(
                        shaderPlatformInterface, request.m_platformInfo, shaderSourceFileFullPath, supervariantIndex, hlslSourcePath);
                    if (hlslCode.empty() || hlslSourcePath.empty())
                    {
                        response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
                        return;
                    }

                    // Setup the shader variant creation context:
                    ShaderVariantCreationContext2 shaderVariantCreationContext =
                    {
                        *shaderPlatformInterface, request.m_platformInfo, buildOptions.m_compilerArguments, request.m_tempDirPath,
                        startTime,
                        shaderSourceDescriptor,
                        *shaderOptionGroupLayout.get(),
                        shaderEntryPoints,
                        Uuid::CreateRandom(),
                        shaderStemNamePrefix,
                        hlslSourcePath, hlslCode
                    };

                    AZStd::optional<RHI::ShaderPlatformInterface::ByProducts> outputByproducts;
                    auto shaderVariantAssetOutcome = CreateShaderVariantAsset(variantInfo, shaderVariantCreationContext, outputByproducts);
                    if (!shaderVariantAssetOutcome.IsSuccess())
                    {
                        AZ_Error(ShaderVariantAssetBuilder2Name, false, "%s\n", shaderVariantAssetOutcome.GetError().c_str());
                        response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
                        return;
                    }
                    Data::Asset<RPI::ShaderVariantAsset2> shaderVariantAsset = shaderVariantAssetOutcome.TakeValue();


                    // Time to save the asset in the tmp folder so it ends up in the Cache folder.
                    const uint32_t productSubID = RPI::ShaderVariantAsset2::MakeAssetProductSubId(
                        shaderPlatformInterface->GetAPIUniqueIndex(), supervariantIndex.GetIndex(),
                        shaderVariantAsset->GetStableId());
                    AssetBuilderSDK::JobProduct assetProduct;
                    if (!SerializeOutShaderVariantAsset(shaderVariantAsset, shaderStemNamePrefix,
                            request.m_tempDirPath, *shaderPlatformInterface, productSubID,
                            assetProduct))
                    {
                        response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
                        return;
                    }
                    response.m_outputProducts.push_back(assetProduct);

                    if (outputByproducts)
                    {
                        // add byproducts as job output products:
                        uint32_t subProductType = RPI::ShaderVariantAsset2::ShaderVariantAsset2SubProductType;
                        for (const AZStd::string& byproduct : outputByproducts.value().m_intermediatePaths)
                        {
                            AssetBuilderSDK::JobProduct jobProduct;
                            jobProduct.m_productFileName = byproduct;
                            jobProduct.m_productAssetType = Uuid::CreateName("DebugInfoByProduct-PdbOrDxilTxt");
                            jobProduct.m_productSubID = RPI::ShaderVariantAsset2::MakeAssetProductSubId(
                                shaderPlatformInterface->GetAPIUniqueIndex(), supervariantIndex.GetIndex(), shaderVariantAsset->GetStableId(),
                                subProductType++);
                            response.m_outputProducts.push_back(AZStd::move(jobProduct));
                        }
                    }
                    supervariantIndexCounter++;
                } // End of supervariant for block
                
            }

            response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Success;
        }

        bool ShaderVariantAssetBuilder2::SerializeOutShaderVariantAsset(
            const Data::Asset<RPI::ShaderVariantAsset2> shaderVariantAsset, const AZStd::string& shaderStemNamePrefix,
            const AZStd::string& tempDirPath,
            const RHI::ShaderPlatformInterface& shaderPlatformInterface, const uint32_t productSubID, AssetBuilderSDK::JobProduct& assetProduct)
        {
            AZStd::string filename = AZStd::string::format(
                "%s_%s_%u.%s", shaderStemNamePrefix.c_str(), shaderPlatformInterface.GetAPIName().GetCStr(),
                shaderVariantAsset->GetStableId().GetIndex(), RPI::ShaderVariantAsset2::Extension);

            AZStd::string assetPath;
            AzFramework::StringFunc::Path::ConstructFull(tempDirPath.c_str(), filename.c_str(), assetPath, true);

            if (!AZ::Utils::SaveObjectToFile(assetPath, AZ::DataStream::ST_BINARY, shaderVariantAsset.Get()))
            {
                AZ_Error(ShaderVariantAssetBuilder2Name, false, "Failed to save Shader Variant Asset to \"%s\"", assetPath.c_str());
                return false;
            }

            assetProduct.m_productSubID = productSubID;
            assetProduct.m_productFileName = assetPath;
            assetProduct.m_productAssetType = azrtti_typeid<RPI::ShaderVariantAsset2>();
            assetProduct.m_dependenciesHandled = true; // This builder has no dependencies to output

            AZ_TracePrintf(ShaderVariantAssetBuilder2Name, "Shader Variant Asset [%s] compiled successfully.\n", assetPath.c_str());
            return true;
        }


        AZ::Outcome<Data::Asset<RPI::ShaderVariantAsset2>, AZStd::string> ShaderVariantAssetBuilder2::CreateShaderVariantAsset(
            const RPI::ShaderVariantListSourceData::VariantInfo& shaderVariantInfo,
            ShaderVariantCreationContext2& creationContext,
            AZStd::optional<RHI::ShaderPlatformInterface::ByProducts>& outputByproducts)
        {
            // Temporary structure used for sorting and caching intermediate results
            struct OptionCache
            {
                AZ::Name m_optionName;
                AZ::Name m_valueName;
                RPI::ShaderOptionIndex m_optionIndex; // Cached m_optionName
                RPI::ShaderOptionValue m_value; // Cached m_valueName
            };
            AZStd::vector<OptionCache> optionList;
            // We can not have more options than the number of options in the layout:
            optionList.reserve(creationContext.m_shaderOptionGroupLayout.GetShaderOptionCount());

            // This loop will validate and cache the indices for each option value:
            for (const auto& shaderOption : shaderVariantInfo.m_options)
            {
                Name optionName{shaderOption.first};
                Name optionValue{shaderOption.second};

                RPI::ShaderOptionIndex optionIndex = creationContext.m_shaderOptionGroupLayout.FindShaderOptionIndex(optionName);
                if (optionIndex.IsNull())
                {
                    return AZ::Failure(AZStd::string::format("Invalid shader option: %s", optionName.GetCStr()));
                }

                const RPI::ShaderOptionDescriptor& option = creationContext.m_shaderOptionGroupLayout.GetShaderOption(optionIndex);
                RPI::ShaderOptionValue value = option.FindValue(optionValue);
                if (value.IsNull())
                {
                    return AZ::Failure(
                        AZStd::string::format("Invalid value (%s) for shader option: %s", optionValue.GetCStr(), optionName.GetCStr()));
                }

                optionList.push_back(OptionCache{optionName, optionValue, optionIndex, value});
            }

            // Create one instance of the shader variant
            RPI::ShaderOptionGroup optionGroup(&creationContext.m_shaderOptionGroupLayout);

            //! Contains the series of #define macro values that define a variant. Can be empty (root variant).
            //! If this string is NOT empty, a new temporary hlsl file will be created that will be the combination
            //! of this string + @m_hlslSourceContent.
            AZStd::string hlslCodeToPrependForVariant;

            // We want to go over all options listed in the variant and set their respective values
            // This loop will populate the optionGroup and m_shaderCodePrefix in order of the option priority
            for (const auto& optionCache : optionList)
            {
                const RPI::ShaderOptionDescriptor& option = creationContext.m_shaderOptionGroupLayout.GetShaderOption(optionCache.m_optionIndex);

                // Assign the option value specified in the variant:
                option.Set(optionGroup, optionCache.m_value);

                // Populate all shader option defines. We have already confirmed they're valid.
                hlslCodeToPrependForVariant += AZStd::string::format(
                    "#define %s_OPTION_DEF %s\n", optionCache.m_optionName.GetCStr(), optionCache.m_valueName.GetCStr());
            }

            AZStd::string variantShaderSourcePath;
            // Check if we need to prepend any code prefix
            if (!hlslCodeToPrependForVariant.empty())
            {
                // Prepend any shader code prefix that we should apply to this variant
                // and save it back to a file.
                AZStd::string variantShaderSourceString(hlslCodeToPrependForVariant);
                variantShaderSourceString += creationContext.m_hlslSourceContent;

                AZStd::string shaderAssetName = AZStd::string::format(
                    "%s_%s_%u.hlsl", creationContext.m_shaderStemNamePrefix.c_str(),
                    creationContext.m_shaderPlatformInterface.GetAPIName().GetCStr(), shaderVariantInfo.m_stableId);
                AzFramework::StringFunc::Path::Join(
                    creationContext.m_tempDirPath.c_str(), shaderAssetName.c_str(), variantShaderSourcePath, true, true);

                auto outcome = Utils::WriteFile(variantShaderSourceString, variantShaderSourcePath);
                if (!outcome.IsSuccess())
                {
                    return AZ::Failure(AZStd::string::format("Failed to create file %s", variantShaderSourcePath.c_str()));
                }
            }
            else
            {
                variantShaderSourcePath = creationContext.m_hlslSourcePath;
            }

            AZ_TracePrintf(ShaderVariantAssetBuilder2Name, "Variant StableId: %u", shaderVariantInfo.m_stableId);
            AZ_TracePrintf(ShaderVariantAssetBuilder2Name, "Variant Shader Options: %s", optionGroup.ToString().c_str());

            const RPI::ShaderVariantStableId shaderVariantStableId{shaderVariantInfo.m_stableId};

            // By this time the optionGroup was populated with all option values for the variant and
            // the m_shaderCodePrefix contains all option related preprocessing macros
            // Let's add the requested variant:
            RPI::ShaderVariantAssetCreator2 variantCreator;
            RPI::ShaderOptionGroup shaderOptions{&creationContext.m_shaderOptionGroupLayout, optionGroup.GetShaderVariantId()};
            variantCreator.Begin(
                creationContext.m_shaderVariantAssetId, optionGroup.GetShaderVariantId(), shaderVariantStableId,
                shaderOptions.IsFullySpecified());

            const AZStd::unordered_map<AZStd::string, RPI::ShaderStageType>& shaderEntryPoints = creationContext.m_shaderEntryPoints;
            for (const auto& shaderEntryPoint : shaderEntryPoints)
            {
                auto shaderEntryName = shaderEntryPoint.first;
                auto shaderStageType = shaderEntryPoint.second;

                AZ_TracePrintf(ShaderVariantAssetBuilder2Name, "Entry Point: %s", shaderEntryName.c_str());
                AZ_TracePrintf(ShaderVariantAssetBuilder2Name, "Begin compiling shader function \"%s\"", shaderEntryName.c_str());

                auto assetBuilderShaderType = ShaderBuilderUtility::ToAssetBuilderShaderType(shaderStageType);

                // Compile HLSL to the platform specific shader.
                RHI::ShaderPlatformInterface::StageDescriptor descriptor;
                bool shaderWasCompiled = creationContext.m_shaderPlatformInterface.CompilePlatformInternal(
                    creationContext.m_platformInfo, variantShaderSourcePath, shaderEntryName, assetBuilderShaderType,
                    creationContext.m_tempDirPath, descriptor, creationContext.m_shaderCompilerArguments);

                if (!shaderWasCompiled)
                {
                    return AZ::Failure(AZStd::string::format("Could not compile the shader function %s", shaderEntryName.c_str()));
                }
                // bubble up the byproducts to the caller by moving them to the context.
                outputByproducts.emplace(AZStd::move(descriptor.m_byProducts));

                RHI::Ptr<RHI::ShaderStageFunction> shaderStageFunction = creationContext.m_shaderPlatformInterface.CreateShaderStageFunction(descriptor);
                variantCreator.SetShaderFunction(ToRHIShaderStage(assetBuilderShaderType), shaderStageFunction);

                if (descriptor.m_byProducts.m_dynamicBranchCount != AZ::RHI::ShaderPlatformInterface::ByProducts::UnknownDynamicBranchCount)
                {
                    AZ_TracePrintf(
                        ShaderVariantAssetBuilder2Name, "Finished compiling shader function. Number of dynamic branches: %u",
                        descriptor.m_byProducts.m_dynamicBranchCount);
                }
                else
                {
                    AZ_TracePrintf(
                        ShaderVariantAssetBuilder2Name, "Finished compiling shader function. Number of dynamic branches: unknown");
                }
            }

            Data::Asset<RPI::ShaderVariantAsset2> shaderVariantAsset;
            variantCreator.End(shaderVariantAsset);
            return AZ::Success(AZStd::move(shaderVariantAsset));
        }

    } // ShaderBuilder
} // AZ