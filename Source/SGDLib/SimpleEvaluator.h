//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
#pragma once

#include "Basics.h"
#include "DataReader.h"
#include "ComputationNode.h"
#include "ComputationNetwork.h"
#include "DataReaderHelpers.h"
#include "TrainingNodes.h" // TODO: we should move the functions that depend on these to the .cpp

#include <vector>
#include <string>
#include <set>

using namespace std;

namespace Microsoft { namespace MSR { namespace CNTK {

// TODO: get rid of dependency on ElemType
template <class ElemType>
class SimpleEvaluator
{
public:
    SimpleEvaluator(ComputationNetworkPtr net, const size_t numMBsToShowResult = 100, const int traceLevel = 0)
        : m_net(net), m_numMBsToShowResult(numMBsToShowResult), m_traceLevel(traceLevel)
    {
    }

    // returns evaluation node values per sample determined by evalNodeNames (which can include both training and eval criterion nodes)
    vector<double> Evaluate(IDataReader* dataReader, const vector<wstring>& evalNodeNames, const size_t mbSize, const size_t testSize = requestDataSize)
    {
        // determine nodes to evaluate
        std::vector<ComputationNodeBasePtr> evalNodes;

        set<ComputationNodeBasePtr> criteriaLogged; // (keeps track ot duplicates to avoid we don't double-log critera)
        if (evalNodeNames.size() == 0)
        {
            fprintf(stderr, "evalNodeNames are not specified, using all the default evalnodes and training criterion nodes.\n");
            if (m_net->EvaluationNodes().empty() && m_net->FinalCriterionNodes().empty())
                InvalidArgument("There is no default evaluation node or training criterion specified in the network.");

            for (const auto& node : m_net->EvaluationNodes())
                if (criteriaLogged.insert(node).second)
                    evalNodes.push_back(node);

            for (const auto& node : m_net->FinalCriterionNodes())
                if (criteriaLogged.insert(node).second)
                    evalNodes.push_back(node);
        }
        else
        {
            for (int i = 0; i < evalNodeNames.size(); i++)
            {
                const auto& node = m_net->GetNodeFromName(evalNodeNames[i]);
                if (!criteriaLogged.insert(node).second)
                    continue;
                if (node->GetSampleLayout().GetNumElements() != 1)
                    InvalidArgument("Criterion nodes to evaluate must have dimension 1x1.");
                evalNodes.push_back(node);
            }
        }

        // initialize eval results
        std::vector<double> evalResults;
        for (int i = 0; i < evalNodes.size(); i++)
            evalResults.push_back((double) 0);

        // allocate memory for forward computation
        m_net->AllocateAllMatrices(evalNodes, {}, nullptr);

        // prepare features and labels
        auto& featureNodes = m_net->FeatureNodes();
        auto& labelNodes = m_net->LabelNodes();

        StreamMinibatchInputs inputMatrices;
        for (auto& node : featureNodes)
            inputMatrices.AddInputMatrix(node->NodeName(), node->ValuePtr());
        for (auto& node : labelNodes)
            inputMatrices.AddInputMatrix(node->NodeName(), node->ValuePtr());

        // evaluate through minibatches
        size_t totalEpochSamples = 0;
        size_t numMBsRun = 0;
        size_t actualMBSize = 0;
        size_t numSamplesLastMBs = 0;
        size_t lastMBsRun = 0; // MBs run before this display

        std::vector<double> evalResultsLastMBs;
        for (int i = 0; i < evalResults.size(); i++)
            evalResultsLastMBs.push_back((ElemType) 0);

        dataReader->StartMinibatchLoop(mbSize, 0, testSize);
        m_net->StartEvaluateMinibatchLoop(evalNodes);

        const size_t numIterationsBeforePrintingProgress = 100;
        size_t numItersSinceLastPrintOfProgress = 0;
        while (DataReaderHelpers::GetMinibatchIntoNetwork<ElemType>(*dataReader, m_net, nullptr, false, false, inputMatrices, actualMBSize))
        {
            ComputationNetwork::BumpEvalTimeStamp(featureNodes);
            ComputationNetwork::BumpEvalTimeStamp(labelNodes);

            // for now since we share the same label masking flag we call this on one node only
            // Later, when we apply different labels on different nodes
            // we need to add code to call this function multiple times, one for each criteria node
            size_t numSamplesWithLabel = m_net->GetNumSamplesWithLabel(actualMBSize);
            for (int i = 0; i < evalNodes.size(); i++)
            {
                m_net->ForwardProp(evalNodes[i]);
                evalResults[i] += (double) evalNodes[i]->Get00Element(); // criterionNode should be a scalar
            }

            totalEpochSamples += numSamplesWithLabel;
            numMBsRun++;

            if (m_traceLevel > 0)
            {
                numSamplesLastMBs += numSamplesWithLabel;

                if (numMBsRun % m_numMBsToShowResult == 0)
                {
                    DisplayEvalStatistics(lastMBsRun + 1, numMBsRun, numSamplesLastMBs, evalNodes, evalResults, evalResultsLastMBs);

                    for (int i = 0; i < evalResults.size(); i++)
                    {
                        evalResultsLastMBs[i] = evalResults[i];
                    }
                    numSamplesLastMBs = 0;
                    lastMBsRun = numMBsRun;
                }
            }


            if (ProgressTracing::GetTracingFlag())
            {
                numItersSinceLastPrintOfProgress++;
                if (numItersSinceLastPrintOfProgress >= numIterationsBeforePrintingProgress)
                {
                    // TODO: For now just print 0.0 instead of calculating actual progress
                    printf("PROGRESS: %.2f%%\n", 0.0f);
                    numItersSinceLastPrintOfProgress = 0;
                }
            }

            // call DataEnd to check if end of sentence is reached
            // datareader will do its necessary/specific process for sentence ending
            dataReader->DataEnd();
        }

        // show last batch of results
        if (m_traceLevel > 0 && numSamplesLastMBs > 0)
        {
            DisplayEvalStatistics(lastMBsRun + 1, numMBsRun, numSamplesLastMBs, evalNodes, evalResults, evalResultsLastMBs);
        }

        // final statistics
        for (int i = 0; i < evalResultsLastMBs.size(); i++)
            evalResultsLastMBs[i] = 0; // clear this since statistics display will subtract the previous value

        fprintf(stderr, "Final Results: ");
        DisplayEvalStatistics(1, numMBsRun, totalEpochSamples, evalNodes, evalResults, evalResultsLastMBs, true);

        for (int i = 0; i < evalResults.size(); i++)
        {
            evalResults[i] /= totalEpochSamples;
        }

        return evalResults;
    }

protected:
    void DisplayEvalStatistics(const size_t startMBNum, const size_t endMBNum, const size_t numSamplesLastMBs,
                               const vector<ComputationNodeBasePtr>& evalNodes,
                               const double evalResults, const double evalResultsLastMBs, bool displayConvertedValue = false)
    {
        vector<double> evaR;
        evaR.push_back(evalResults);
        vector<double> evaLast;
        evaLast.push_back(evalResultsLastMBs);

        DisplayEvalStatistics(startMBNum, endMBNum, numSamplesLastMBs, evalNodes, evaR, evaLast, displayConvertedValue);
    }

    void DisplayEvalStatistics(const size_t startMBNum, const size_t endMBNum, const size_t numSamplesLastMBs, const vector<ComputationNodeBasePtr>& evalNodes,
                               const vector<double>& evalResults, const vector<double>& evalResultsLastMBs, bool displayConvertedValue = false)
    {
        fprintf(stderr, "Minibatch[%lu-%lu]: SamplesSeen = %lu    ", startMBNum, endMBNum, numSamplesLastMBs);

        for (size_t i = 0; i < evalResults.size(); i++)
        {
            double eresult = (evalResults[i] - evalResultsLastMBs[i]) / numSamplesLastMBs;
            fprintf(stderr, "%ls: %ls/Sample = %.8g    ", evalNodes[i]->NodeName().c_str(), evalNodes[i]->OperationName().c_str(), eresult);

            if (displayConvertedValue)
            {
                // display Perplexity as well for crossEntropy values
                if (evalNodes[i]->OperationName() == OperationNameOf(CrossEntropyWithSoftmaxNode) ||
                    evalNodes[i]->OperationName() == OperationNameOf(CrossEntropyNode) ||
                    evalNodes[i]->OperationName() == OperationNameOf(ClassBasedCrossEntropyWithSoftmaxNode) ||
                    evalNodes[i]->OperationName() == OperationNameOf(NoiseContrastiveEstimationNode))
                    fprintf(stderr, "Perplexity = %.8g    ", std::exp(eresult));
            }
        }

        fprintf(stderr, "\n");
    }

protected:
    ComputationNetworkPtr m_net;
    size_t m_numMBsToShowResult;
    int m_traceLevel;
    void operator=(const SimpleEvaluator&); // (not assignable)
};
} } }
