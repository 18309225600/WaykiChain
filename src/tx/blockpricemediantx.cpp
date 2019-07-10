// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include "blockpricemediantx.h"
#include "main.h"


bool CBlockPriceMedianTx::CheckTx(int nHeight, CCacheWrapper &cw, CValidationState &state) {
    IMPLEMENT_CHECK_TX_REGID(txUid.type());
    return true;
}
/**
 *  force settle/liquidate any under-collateralized CDP (collateral ratio <= 100%)
 */
bool CBlockPriceMedianTx::ExecuteTx(int nHeight, int nIndex, CCacheWrapper &cw, CValidationState &state) {
    cw.txUndo.txid = GetHash();

    CAccount fcoinGenesisAccount;
    cw.accountCache.GetFcoinGenesisAccount(fcoinGenesisAccount);
    uint64_t currRiskReserveScoins = fcoinGenesisAccount.scoins;
    CAccountLog fcoinGenesisAcctLog(fcoinGenesisAccount); //save account state before modification

    //0. Check Global Collateral Ratio floor & Collateral Ceiling if reached
     if (cw.cdpCache.CheckGlobalCollateralFloorReached(cw.ppCache.GetBcoinMedianPrice())) {
        LogPrint("CDP", "CBlockPriceMedianTx::ExecuteTx, GlobalCollateralFloorReached!!");
        return true;
    }
    if (cw.cdpCache.CheckGlobalCollateralCeilingReached(bcoinsToStake)) {
        LogPrint("CDP", "CBlockPriceMedianTx::ExecuteTx, GlobalCollateralCeilingReached!!");
        return true;
    }

    //1. get all CDPs to be force settled
    set<CUserCDP> forceLiquidateCdps;
    uint64_t bcoinMedianPrice = cw.ppCache.GetBcoinMedianPrice();
    cw.cdpCache.cdpMemCache.GetCdpListByCollateralRatio(
                                    cw.cdpCache.GetDefaultForceLiquidateRatio(),
                                    bcoinMedianPrice,
                                    forceLiquidateCdps));

    //2. force settle each cdp
    for (const auto &cdp : forceLiquidateCdps) {
        LogPrint("CDP", "CBlockPriceMedianTx::ExecuteTx, begin to force settle CDP (%s)", cdp.ToString());
        if (currRiskReserveScoins < cdp.totalOwedScoins) {
            LogPrint("CDP", "CBlockPriceMedianTx::ExecuteTx, currRiskReserveScoins(%lu) < cdp.totalOwedScoins(%lu) !!",
                    currRiskReserveScoins, cdp.totalOwedScoins);
            break;
        }

        // a) minus scoins from the risk reserve pool to repay CDP scoins
        uint64_t prevRiskReserveScoins = currRiskReserveScoins;
        currRiskReserveScoins -= cdp.totalOwedScoins;

        // b) sell WICC for WUSD to return to risk reserve pool
        auto pBcoinSellMarketOrder = CDEXSysOrder::CreateSellMarketOrder(CoinType::WUSD, AssetType::WICC, cdp.totalStakedBcoins);
        if (!cw.dexCache.CreateSysOrder(GetHash(), *pBcoinSellMarketOrder, cw.txUndo.dbOpLogMap)) {
            return state.DoS(100, ERRORMSG("CBlockPriceMedianTx::ExecuteTx: SellBcoinForScoin, create system buy order failed"),
                            CREATE_SYS_ORDER_FAILED, "create-sys-order-failed");
        }

        // c) inflate WGRT coins and sell them for WUSD to return to risk reserve pool
        assert( cdp.totalOwedScoins >  cdp.totalStakedBcoins * bcoinMedianPrice);
        uint64_t fcoinsValueToInflate = cdp.totalOwedScoins - cdp.totalStakedBcoins * bcoinMedianPrice;
        uint64_t fcoinsToInflate = fcoinsValueToInflate / cw.ppCache.GetFcoinMedianPrice();
        auto pFcoinSellMarketOrder = CDEXSysOrder::CreateSellMarketOrder(CoinType::WUSD, AssetType::WGRT, fcoinsToInflate);
        if (!cw.dexCache.CreateSysOrder(GetHash(), *pFcoinSellMarketOrder, cw.txUndo.dbOpLogMap)) {
            return state.DoS(100, ERRORMSG("CBlockPriceMedianTx::ExecuteTx: SellFcoinForScoin, create system buy order failed"),
                            CREATE_SYS_ORDER_FAILED, "create-sys-order-failed");
        }

        // d) Close the CDP
        cw.cdpCache.EraseCdp(cdp, cw.txUndo.dbOpLogMap);
        LogPrint("CDP", "CBlockPriceMedianTx::ExecuteTx, Force settled CDP: "
                "Placed BcoinSellMarketOrder:  %s\n"
                "Placed FcoinSellMarketOrder:  %s\n"
                "prevRiskReserveScoins: %lu -> currRiskReserveScoins: %lu\n",
                pBcoinSellMarketOrder.ToString(),
                pFcoinSellMarketOrder.ToString(),
                prevRiskReserveScoins,
                currRiskReserveScoins);

        //TODO: double check state consistence between MemCach & DBCache for CDP
    }

    fcoinGenesisAccount.scoins = currRiskReserveScoins;
    cw.accountCache.SaveAccount(fcoinGenesisAccount)
    cw.txUndo.accountLogs.push_back(fcoinGenesisAcctLog);

    return true;
}

bool CBlockPriceMedianTx::UndoExecuteTx(int nHeight, int nIndex, CCacheWrapper &cw, CValidationState &state) {
    // Nothing to do here.
    return true;
}

string CBlockPriceMedianTx::ToString(CAccountDBCache &accountCache) {
    string pricePoints;
    for (auto it = mapMedianPricePoints.begin(); it != mapMedianPricePoints.end(); ++it) {
        pricePoints += strprintf("{coin_type:%u, price_type:%u, price:%lld}",
                        it->first.coinType, it->first.priceType, it->second);
    };

    string str = strprintf(
        "txType=%s, hash=%s, ver=%d, nValidHeight=%d, txUid=%s, llFees=%ld,"
        "median_price_points=%s\n",
        GetTxType(nTxType), GetHash().GetHex(), nVersion, nValidHeight, txUid.ToString(), llFees,
        pricePoints);

    return str;
}

Object CBlockPriceMedianTx::ToJson(const CAccountDBCache &accountCache) const {
    Object result;

    CKeyID keyId;
    accountCache.GetKeyId(txUid, keyId);

    Array pricePointArray;
    for (auto it = mapMedianPricePoints.begin(); it != mapMedianPricePoints.end(); ++it) {
        Object subItem;
        subItem.push_back(Pair("coin_type",     it->first.coinType));
        subItem.push_back(Pair("price_type",    it->first.priceType));
        subItem.push_back(Pair("price",         it->second));
        pricePointArray.push_back(subItem);
    };

    result.push_back(Pair("hash",           GetHash().GetHex()));
    result.push_back(Pair("tx_type",        GetTxType(nTxType)));
    result.push_back(Pair("ver",            nVersion));
    result.push_back(Pair("tx_uid",         txUid.ToString()));
    result.push_back(Pair("addr",           keyId.ToAddress()));
    result.push_back(Pair("valid_height",   nValidHeight));
    result.push_back(Pair("fees",           llFees));

    result.push_back(Pair("median_price_points",   pricePointArray));

    return result;
}

bool CBlockPriceMedianTx::GetInvolvedKeyIds(CCacheWrapper &cw, set<CKeyID> &keyIds) {
    //TODO
    return true;
}

map<CCoinPriceType, uint64_t> CBlockPriceMedianTx::GetMedianPrice() const {
    return mapMedianPricePoints;
}
