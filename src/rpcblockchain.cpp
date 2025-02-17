// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpcserver.h>
#include <main.h>
#include <kernel.h>
#include <checkpoints.h>


extern void TxToJSON(const CTransaction& tx, const uint256 hashBlock, json_spirit::Object& entry);

double GetDifficulty(const CBlockIndex* blockindex)
{
    // Floating point number that is a multiple of the minimum difficulty,
    // minimum difficulty = 1.0.
    if (blockindex == nullptr)
    {
        if (pindexBest == nullptr)
            return 1.0;
        else
            blockindex = GetLastBlockIndex(pindexBest, false);
    }

    int nShift = (blockindex->nBits >> 24) & 0xff;

    double dDiff =
        (double)0x0000ffff / (double)(blockindex->nBits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

double GetPoWMHashPS()
{
    if (pindexBest->nHeight >= Params().LastPOWBlock())
        return 0;

    int nPoWInterval = 72;
    int64_t nTargetSpacingWorkMin = 30, nTargetSpacingWork = 30;

    CBlockIndex* pindex = pindexGenesisBlock;
    CBlockIndex* pindexPrevWork = pindexGenesisBlock;

    while (pindex)
    {
        if (pindex->IsProofOfWork())
        {
            int64_t nActualSpacingWork = pindex->GetBlockTime() - pindexPrevWork->GetBlockTime();
            nTargetSpacingWork = ((nPoWInterval - 1) * nTargetSpacingWork + nActualSpacingWork + nActualSpacingWork) / (nPoWInterval + 1);
            nTargetSpacingWork = std::max(nTargetSpacingWork, nTargetSpacingWorkMin);
            pindexPrevWork = pindex;
        }

        pindex = pindex->pnext;
    }

    return GetDifficulty() * 4294.967296 / nTargetSpacingWork;
}

double GetPoSKernelPS()
{
    int nPoSInterval = 72;
    double dStakeKernelsTriedAvg = 0;
    int nStakesHandled = 0, nStakesTime = 0;

    CBlockIndex* pindex = pindexBest;;
    CBlockIndex* pindexPrevStake = nullptr;

    while (pindex && nStakesHandled < nPoSInterval)
    {
        if (pindex->IsProofOfStake())
        {
            if (pindexPrevStake)
            {
                dStakeKernelsTriedAvg += GetDifficulty(pindexPrevStake) * 4294967296.0;
                nStakesTime += pindexPrevStake->nTime - pindex->nTime;
                nStakesHandled++;
            }
            pindexPrevStake = pindex;
        }

        pindex = pindex->pprev;
    }

    double result = 0;

    if (nStakesTime)
        result = dStakeKernelsTriedAvg / nStakesTime * STAKE_TIMESTAMP_MASK + 1;

    return result;
}

json_spirit::Object blockToJSON(const CBlock& block, const CBlockIndex* blockindex, bool fPrintTransactionDetail)
{
    json_spirit::Object result;
    result.push_back(json_spirit::Pair("hash", block.GetHash().GetHex()));
    int confirmations = -1;
    // Only report confirmations if the block is on the main chain
    if (blockindex->IsInMainChain())
        confirmations = nBestHeight - blockindex->nHeight + 1;
    result.push_back(json_spirit::Pair("confirmations", confirmations));
    result.push_back(json_spirit::Pair("size", (int)::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION)));
    result.push_back(json_spirit::Pair("height", blockindex->nHeight));
    result.push_back(json_spirit::Pair("version", block.nVersion));
    result.push_back(json_spirit::Pair("merkleroot", block.hashMerkleRoot.GetHex()));
    result.push_back(json_spirit::Pair("mint", ValueFromAmount(blockindex->nMint)));
    result.push_back(json_spirit::Pair("time", (int64_t)block.GetBlockTime()));
    result.push_back(json_spirit::Pair("nonce", (uint64_t)block.nNonce));
    result.push_back(json_spirit::Pair("bits", strprintf("%08x", block.nBits)));
    result.push_back(json_spirit::Pair("difficulty", GetDifficulty(blockindex)));
    result.push_back(json_spirit::Pair("blocktrust", leftTrim(blockindex->GetBlockTrust().GetHex(), '0')));
    result.push_back(json_spirit::Pair("chaintrust", leftTrim(blockindex->nChainTrust.GetHex(), '0')));
    if (blockindex->pprev)
        result.push_back(json_spirit::Pair("previousblockhash", blockindex->pprev->GetBlockHash().GetHex()));
    if (blockindex->pnext)
        result.push_back(json_spirit::Pair("nextblockhash", blockindex->pnext->GetBlockHash().GetHex()));

    result.push_back(json_spirit::Pair("flags", strprintf("%s%s", blockindex->IsProofOfStake()? "proof-of-stake" : "proof-of-work", blockindex->GeneratedStakeModifier()? " stake-modifier": "")));
    result.push_back(json_spirit::Pair("proofhash", blockindex->hashProof.GetHex()));
    result.push_back(json_spirit::Pair("entropybit", (int)blockindex->GetStakeEntropyBit()));
    result.push_back(json_spirit::Pair("modifier", strprintf("%016x", blockindex->nStakeModifier)));
    result.push_back(json_spirit::Pair("modifierv2", blockindex->bnStakeModifierV2.GetHex()));
    json_spirit::Array txinfo;
    for (const CTransaction& tx : block.vtx)
    {
        if (fPrintTransactionDetail)
        {
            json_spirit::Object entry;

            entry.push_back(json_spirit::Pair("txid", tx.GetHash().GetHex()));
            TxToJSON(tx, 0, entry);

            txinfo.push_back(entry);
        }
        else
            txinfo.push_back(tx.GetHash().GetHex());
    }

    result.push_back(json_spirit::Pair("tx", txinfo));

    if (block.IsProofOfStake())
        result.push_back(json_spirit::Pair("signature", HexStr(block.vchBlockSig.begin(), block.vchBlockSig.end())));

    return result;
}

json_spirit::Value getbestblockhash(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "getbestblockhash\n"
            "Returns the hash of the best block in the longest block chain.");

    return hashBestChain.GetHex();
}

json_spirit::Value getblockcount(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "getblockcount\n"
            "Returns the number of blocks in the longest block chain.");

    return nBestHeight;
}


json_spirit::Value getdifficulty(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "getdifficulty\n"
            "Returns the difficulty as a multiple of the minimum difficulty.");

    json_spirit::Object obj;
    obj.push_back(json_spirit::Pair("proof-of-work",        GetDifficulty()));
    obj.push_back(json_spirit::Pair("proof-of-stake",       GetDifficulty(GetLastBlockIndex(pindexBest, true))));
    return obj;
}


json_spirit::Value getrawmempool(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "getrawmempool\n"
            "Returns all transaction ids in memory pool.");

    std::vector<uint256> vtxid;
    mempool.queryHashes(vtxid);

    json_spirit::Array a;
    for (const uint256& hash : vtxid)
        a.push_back(hash.ToString());

    return a;
}

json_spirit::Value getblockhash(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "getblockhash <index>\n"
            "Returns hash of block in best-block-chain at <index>.");

    int nHeight = params[0].get_int();
    if (nHeight < 0 || nHeight > nBestHeight)
        throw std::runtime_error("Block number out of range.");

    CBlockIndex* pblockindex = FindBlockByHeight(nHeight);
    return pblockindex->phashBlock->GetHex();
}

json_spirit::Value getblock(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw std::runtime_error(
            "getblock <hash> [txinfo]\n"
            "txinfo optional to print more detailed tx info\n"
            "Returns details of a block with given block-hash.");

    std::string strHash = params[0].get_str();
    uint256 hash(strHash);

    if (mapBlockIndex.count(hash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hash];
    block.ReadFromDisk(pblockindex, true);

    return blockToJSON(block, pblockindex, params.size() > 1 ? params[1].get_bool() : false);
}

json_spirit::Value getblockbynumber(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw std::runtime_error(
            "getblockbynumber <number> [txinfo]\n"
            "txinfo optional to print more detailed tx info\n"
            "Returns details of a block with given block-number.");

    int nHeight = params[0].get_int();
    if (nHeight < 0 || nHeight > nBestHeight)
        throw std::runtime_error("Block number out of range.");

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hashBestChain];
    while (pblockindex->nHeight > nHeight)
        pblockindex = pblockindex->pprev;

    uint256 hash = *pblockindex->phashBlock;

    pblockindex = mapBlockIndex[hash];
    block.ReadFromDisk(pblockindex, true);

    return blockToJSON(block, pblockindex, params.size() > 1 ? params[1].get_bool() : false);
}

// ppcoin: get information of sync-checkpoint
json_spirit::Value getcheckpoint(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "getcheckpoint\n"
            "Show info of synchronized checkpoint.\n");

    json_spirit::Object result;
    const CBlockIndex* pindexCheckpoint = Checkpoints::AutoSelectSyncCheckpoint();

    result.push_back(json_spirit::Pair("synccheckpoint", pindexCheckpoint->GetBlockHash().ToString().c_str()));
    result.push_back(json_spirit::Pair("height", pindexCheckpoint->nHeight));
    result.push_back(json_spirit::Pair("timestamp", DateTimeStrFormat(pindexCheckpoint->GetBlockTime()).c_str()));

    result.push_back(json_spirit::Pair("policy", "rolling"));

    return result;
}
