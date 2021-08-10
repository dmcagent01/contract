/**
 *  @file
 *  @copyright defined in dmc/LICENSE.txt
 */

#include <dmc.token/utils.hpp>
#include <dmc.token/dmc.token.hpp>

namespace eosio {

void token::bill(name owner, extended_asset asset, double price, time_point_sec expire_on, uint64_t deposit_ratio, string memo)
{
    require_auth(owner);
    check(memo.size() <= 256, "memo has more than 256 bytes");
    extended_symbol s_sym = asset.get_extended_symbol();
    check(s_sym == pst_sym, "only proof of service token can be billed");
    check(price >= 0.0001 && price < std::pow(2, 32), "invalid price");
    check(asset.quantity.amount > 0, "must bill a positive amount");
    check(deposit_ratio >= 0, "invalid deposit ratio");

    time_point_sec now_time = time_point_sec(current_time_point());
    check(expire_on >= now_time + get_dmc_config("serverinter"_n, default_service_interval), "invalid service time");

    uint64_t price_t = price * std::pow(2, 32);
    sub_balance(owner, asset);
    bill_stats sst(get_self(), owner.value);

    auto hash = sha256<stake_id_args>({ owner, asset, price_t, time_point_sec(current_time_point()), memo });
    uint64_t bill_id = uint64_t(*reinterpret_cast<const uint64_t*>(&hash));

    sst.emplace(_self, [&](auto& r) {
        r.primary = sst.available_primary_key();
        r.bill_id = bill_id;
        r.owner = owner;
        r.unmatched = asset;
        r.matched = extended_asset(0, pst_sym);
        r.price = price_t;
        r.created_at = now_time;
        r.updated_at = now_time;
        r.expire_on = expire_on;
        r.deposit_ratio = deposit_ratio;
    });
    SEND_INLINE_ACTION(*this, billrec, { _self, "active"_n }, { owner, asset, bill_id, BILL });
}

void token::unbill(name owner, uint64_t bill_id, string memo)
{
    require_auth(owner);
    check(memo.size() <= 256, "memo has more than 256 bytes");
    bill_stats sst(get_self(), owner.value);
    auto ust_idx = sst.get_index<"byid"_n>();
    auto ust = ust_idx.find(bill_id);
    check(ust != ust_idx.end(), "no such record");
    extended_asset unmatched_asseet = ust->unmatched;
    calbonus(owner, bill_id, owner);
    ust_idx.erase(ust);
    add_balance(owner, unmatched_asseet, owner);

    SEND_INLINE_ACTION(*this, billrec, { _self, "active"_n }, { owner, unmatched_asseet, bill_id, UNBILL });
}

void token::order(name owner, name miner, uint64_t bill_id, extended_asset asset, extended_asset reserve, string memo, time_point_sec deposit_valid)
{
    require_auth(owner);
    check(memo.size() <= 256, "memo has more than 256 bytes");
    check(owner != miner, "owner and user are same person");
    check(asset.get_extended_symbol() == pst_sym, "only proof of service token can be ordered");
    check(asset.quantity.amount > 0, "must order a positive amount");
    check(reserve.quantity.amount >= 0, "reserve amount must >= 0");
    require_recipient(owner);
    require_recipient(miner);

    bill_stats sst(get_self(), miner.value);
    auto ust_idx = sst.get_index<"byid"_n>();
    auto ust = ust_idx.find(bill_id);
    check(ust != ust_idx.end(), "no such record");
    check(ust->unmatched >= asset, "overdrawn balance");

    uint64_t order_serivce_epoch = get_dmc_config("ordsrvepoch"_n, default_order_service_epoch);
    check(deposit_valid <= ust->expire_on, "service has expired");
    check(deposit_valid >= time_point_sec(current_time_point()) + eosio::seconds(order_serivce_epoch), 
        "service not reach minimum deposit expire time");

    double price = (double)ust->price / std::pow(2, 32);
    double dmc_amount = price * asset.quantity.amount;
    extended_asset user_to_pay = get_asset_by_amount<double, std::ceil>(dmc_amount, dmc_sym);

    // deposit
    extended_asset user_to_deposit = extended_asset(std::round(user_to_pay.quantity.amount * ust->deposit_ratio), dmc_sym);
    check(reserve >= user_to_pay + user_to_deposit, "reserve can't pay first time");
    sub_balance(owner, reserve);

    uint64_t now_time_t = calbonus(miner, bill_id, owner);

    ust_idx.modify(ust, get_self(), [&](auto& s) {
        s.unmatched -= asset;
        s.matched += asset;
        s.updated_at = time_point_sec(now_time_t);
    });

    uint64_t claims_interval = get_dmc_config("claiminter"_n, default_dmc_claims_interval);

    dmc_orders order_tbl(get_self(), get_self().value);
    auto hash = sha256<order_id_args>({ owner, miner, bill_id, asset, reserve, memo, time_point_sec(current_time_point()) });
    uint64_t order_id = uint64_t(*reinterpret_cast<const uint64_t*>(&hash));
    while (order_tbl.find(order_id) != order_tbl.end()) {
        order_id += 1;
    }
    
    dmc_makers maker_tbl(get_self(), get_self().value);
    auto maker_iter = maker_tbl.find(miner.value);
    check(maker_iter != maker_tbl.end(), "can't find maker pool");
    // sub_stats(asset);
    // change_pst(miner, -asset);
    double benchmark_stake_rate = get_dmc_rate(maker_iter->benchmark_stake_rate);
    double r = cal_current_rate(maker_iter->total_staked, miner);
    auto miner_lock_dmc = extended_asset(user_to_pay.quantity.amount * r, user_to_pay.get_extended_symbol());
    maker_tbl.modify(maker_iter, owner, [&](auto& m) {
        m.current_rate = cal_current_rate(m.total_staked - miner_lock_dmc, miner);
        //m.total_staked -= miner_lock_dmc;
    });
    dmc_order order_info = {
        .order_id = order_id,
        .user = owner,
        .miner = miner,
        .bill_id = bill_id,
        .user_pledge = reserve - user_to_pay - user_to_deposit,
        .miner_lock_pst = asset,
        .miner_lock_dmc = miner_lock_dmc,
        .settlement_pledge = extended_asset(0, user_to_pay.get_extended_symbol()),
        .lock_pledge = user_to_pay,
        .price = user_to_pay,
        .state = OrderStateWaiting,
        .deliver_start_date = time_point_sec(),
        .latest_settlement_date = time_point_sec(),
        .miner_lock_rsi = extended_asset(0, rsi_sym),
        .miner_rsi = extended_asset(0, rsi_sym),
        .user_rsi = extended_asset(0, rsi_sym),
        .deposit = user_to_deposit,
        .deposit_valid = deposit_valid,
        .cancel_date =  time_point_sec()
    };
    order_tbl.emplace(owner, [&](auto& o) {
        o = order_info;
    });

    dmc_challenge challenge_info = {
        .order_id = order_id,
        .pre_merkle_root = checksum256(),
        .pre_data_block_count = 0,
        .merkle_submitter = get_self(),
        .challenge_times = 0,
        .state = ChallengePrepare,
        .user_lock = extended_asset(0, dmc_sym),
        .miner_pay = extended_asset(0, dmc_sym),
    };
    dmc_challenges challenge_tbl(get_self(), get_self().value);
    challenge_tbl.emplace(owner, [&](auto& c) {
        c = challenge_info;
    });

    if (reserve.quantity.amount > 0) {
        extended_asset zero_dmc = extended_asset(0, dmc_sym);
        token::ordercharec_action ordercharec_act{ token_account, { _self, active_permission } };
        ordercharec_act.send(order_id, reserve, zero_dmc, zero_dmc, zero_dmc, time_point_sec(current_time_point()), OrderReceiptUser);
    }
    generate_maker_snapshot(order_info.order_id, bill_id, order_info.miner, owner);
    
    trace_price_history(price, bill_id);
    token::orderrec_action orderrec_act{ token_account, { _self, active_permission } };
    orderrec_act.send(order_info, 1);
    token::challengerec_action challengerec_act{ token_account, { _self, active_permission } };
    challengerec_act.send(challenge_info);
}

void token::increase(name owner, extended_asset asset, name miner)
{
    require_auth(owner);
    check(asset.get_extended_symbol() == dmc_sym, "only DMC can be staked");
    check(asset.quantity.amount > 0, "must increase a positive amount");

    sub_balance(owner, asset);

    dmc_makers maker_tbl(get_self(), get_self().value);
    auto iter = maker_tbl.find(miner.value);
    dmc_maker_pool dmc_pool(get_self(), miner.value);
    auto p_iter = dmc_pool.find(owner.value);
    if (iter == maker_tbl.end()) {
        if (owner == miner) {
            maker_tbl.emplace(miner, [&](auto& m) {
                m.miner = owner;
                m.current_rate = cal_current_rate(asset, miner);
                m.miner_rate = 1;
                m.total_weight = static_weights;
                m.total_staked = asset;
                m.benchmark_stake_rate = get_dmc_config("bmrate"_n, default_benchmark_stake_rate);
            });

            SEND_INLINE_ACTION(*this, makercharec, { _self, "active"_n }, { owner, miner, asset, MakerReceiptIncrease });
            dmc_pool.emplace(owner, [&](auto& p) {
                p.owner = owner;
                p.weight = static_weights;
            });
        } else {
            check(false, "no such record");
        }
    } else {
        extended_asset new_total = iter->total_staked + asset;
        double new_weight = (double)asset.quantity.amount / iter->total_staked.quantity.amount * iter->total_weight;
        double total_weight = iter->total_weight + new_weight;
        check(new_weight > 0, "invalid new weight");
        check(new_weight / total_weight > 0.0001, "increase too lower");

        double r = cal_current_rate(new_total, miner);
        maker_tbl.modify(iter, get_self(), [&](auto& m) {
            m.total_weight = total_weight;
            m.total_staked = new_total;
            m.current_rate = r;
        });

        SEND_INLINE_ACTION(*this, makercharec, { _self, "active"_n }, { owner, miner, asset, MakerReceiptIncrease });

        if (p_iter != dmc_pool.end()) {
            dmc_pool.modify(p_iter, get_self(), [&](auto& s) {
                s.weight += new_weight;
            });
        } else {
            dmc_pool.emplace(owner, [&](auto& p) {
                p.owner = owner;
                p.weight = new_weight;
            });
        }

        auto miner_iter = dmc_pool.find(miner.value);
        check(miner_iter != dmc_pool.end(), ""); // never happened
        check(miner_iter->weight / total_weight >= iter->miner_rate, "exceeding the maximum rate");
    }
}

void token::redemption(name owner, double rate, name miner)
{
    require_auth(owner);

    check(rate > 0 && rate <= 1, "invalid rate");
    dmc_makers maker_tbl(get_self(), get_self().value);
    auto iter = maker_tbl.find(miner.value);
    check(iter != maker_tbl.end(), "no such record");

    dmc_maker_pool dmc_pool(get_self(), miner.value);
    auto p_iter = dmc_pool.find(owner.value);
    check(p_iter != dmc_pool.end(), "no such limit partnership");

    double owner_weight = p_iter->weight * rate;
    double rede_rate = owner_weight / iter->total_weight;
    extended_asset rede_quantity = extended_asset(std::floor(iter->total_staked.quantity.amount * rede_rate), dmc_sym);

    bool last_one = false;
    if (rate == 1) {
        dmc_pool.erase(p_iter);
        auto pool_begin = dmc_pool.begin();
        if (pool_begin == dmc_pool.end()) {
            rede_quantity = iter->total_staked;
        } else if (++pool_begin == dmc_pool.end()) {
            last_one = true;
            pool_begin--;
            owner_weight = pool_begin->weight;
        }
    } else {
        dmc_pool.modify(p_iter, get_self(), [&](auto& s) {
            s.weight -= owner_weight;
        });
        check(p_iter->weight > 0, "negative pool weight amount");
    }

    double total_weight = iter->total_weight - owner_weight;
    extended_asset total_staked = iter->total_staked - rede_quantity;
    double benchmark_stake_rate = get_dmc_rate(iter->benchmark_stake_rate);
    double r = cal_current_rate(total_staked, miner);

    if (miner == owner) {
        check(r >= benchmark_stake_rate, "current stake rate less than benchmark stake rate, redemption fails");

        double miner_rate = 0.0;
        if (total_staked.quantity.amount == 0) {
            miner_rate = uint64_max;
        } else {
            auto miner_iter = dmc_pool.find(miner.value);
            check(miner_iter != dmc_pool.end(), "miner can only redeem all last");
            miner_rate = miner_iter->weight / total_weight;
        }
        check(miner_rate >= iter->miner_rate, "below the minimum rate");
    }
    check(rede_quantity.quantity.amount > 0, "dust attack detected");
    lock_add_balance(owner, rede_quantity, time_point_sec(current_time_point() +  eosio::days(3 * day_sec)), owner);
    SEND_INLINE_ACTION(*this, redeemrec, { get_self(), "active"_n }, { owner, miner, rede_quantity });
    if (total_staked.quantity.amount == 0) {
        maker_tbl.erase(iter);
    } else {
        maker_tbl.modify(iter, get_self(), [&](auto& m) {
            m.total_weight = total_weight;
            m.total_staked = total_staked;
            m.current_rate = r;
            if (last_one)
                m.total_weight = owner_weight;
        });
        check(iter->total_staked.quantity.amount >= 0, "negative total_staked amount");
        check(iter->total_weight >= 0, "negative total weight amount");
    }

    SEND_INLINE_ACTION(*this, makercharec, { _self, "active"_n }, { owner, miner, -rede_quantity, MakerReceiptRedemption });

    if (rate != 1)
        check(p_iter->weight / iter->total_weight > 0.0001, "The remaining weight is too low");
}

void token::mint(name owner, extended_asset asset)
{
    require_auth(owner);
    check(asset.quantity.amount > 0, "must mint a positive amount");
    check(asset.get_extended_symbol() == pst_sym, "only PST can be minted");

    dmc_makers maker_tbl(get_self(), get_self().value);
    const auto& iter = maker_tbl.get(owner.value, "no such pst maker");

    double benchmark_stake_rate = get_dmc_rate(iter.benchmark_stake_rate);
    double makerd_pst = (double)get_real_asset(iter.total_staked) / benchmark_stake_rate;
    extended_asset added_asset = asset;
    pststats pst_acnts(get_self(), get_self().value);

    auto st = pst_acnts.find(owner.value);
    if (st != pst_acnts.end())
        added_asset += st->amount;

    check(std::floor(makerd_pst) >= added_asset.quantity.amount, "insufficient funds to mint");

    add_stats(asset);
    add_balance(owner, asset, owner);
    change_pst(owner, asset);
    double r = cal_current_rate(iter.total_staked, owner);
    check(r >= benchmark_stake_rate, "current stake rate less than benchmark stake rate, mint fails");

    maker_tbl.modify(iter, get_self(), [&](auto& m) {
        m.current_rate = r;
    });
}

void token::setmakerrate(name owner, double rate)
{
    require_auth(owner);
    check(rate >= 0.2 && rate <= 1, "invalid rate");
    dmc_makers maker_tbl(get_self(), get_self().value);
    const auto& iter = maker_tbl.get(owner.value, "no such record");

    dmc_maker_pool dmc_pool(get_self(), owner.value);
    auto miner_iter = dmc_pool.find(owner.value);
    check(miner_iter != dmc_pool.end(), "miner can not destroy maker"); // never happened
    check(miner_iter->weight / iter.total_weight >= rate, "rate does not meet limits");

    maker_tbl.modify(iter, get_self(), [&](auto& s) {
        s.miner_rate = rate;
    });
}

void token::setmakerbstr(name owner, uint64_t self_benchmark_stake_rate)
{
    require_auth(owner);
    dmc_makers maker_tbl(get_self(), get_self().value);
    const auto& iter = maker_tbl.get(owner.value, "no such record");
    dmc_maker_pool dmc_pool(get_self(), owner.value);
    auto miner_iter = dmc_pool.find(owner.value);
    check(miner_iter != dmc_pool.end(), "miner can not destroy maker"); // never happened
    time_point_sec now = time_point_sec(current_time_point());

    check(now >= iter.rate_updated_at + maker_change_rate_interval, "change rate interval too short");

    // no limit if set up rate first time, only >= m
    if (iter.rate_updated_at == time_point_sec(0)) {
        check(self_benchmark_stake_rate >= get_dmc_config("bmrate"_n, default_benchmark_stake_rate), "invalid benchmark_stake_rate");
    } else {
        check(self_benchmark_stake_rate <= iter.benchmark_stake_rate * 1.1
                && self_benchmark_stake_rate >= iter.benchmark_stake_rate * 0.9
                && self_benchmark_stake_rate >= get_dmc_config("bmrate"_n, default_benchmark_stake_rate),
            "invalid benchmark_stake_rate");
    }

    maker_tbl.modify(iter, get_self(), [&](auto& s) {
        s.benchmark_stake_rate = self_benchmark_stake_rate;
        s.rate_updated_at = now;
    });
}

double token::cal_current_rate(extended_asset dmc_asset, name owner)
{
    pststats pst_acnts(get_self(), get_self().value);
    double r = 0.0;
    auto st = pst_acnts.find(owner.value);
    if (st != pst_acnts.end() && st->amount.quantity.amount != 0) {
        r = (double)get_real_asset(dmc_asset) / st->amount.quantity.amount;
    } else {
        r = uint64_max;
    }
    return r;
}

void token::liquidation(string memo)
{
    require_auth(eos_account);
    dmc_makers maker_tbl(get_self(), get_self().value);
    auto maker_idx = maker_tbl.get_index<"byrate"_n>();

    pststats pst_acnts(get_self(), get_self().value);
    constexpr uint64_t required_size = 20;
    std::vector<std::tuple<name /* miner */, extended_asset /* pst_asset */, extended_asset /* dmc_asset */>> liquidation_required;
    liquidation_required.reserve(required_size);
    for (auto maker_it = maker_idx.cbegin(); maker_it != maker_idx.cend() && maker_it->current_rate < get_dmc_rate(maker_it->get_n()) && liquidation_required.size() < required_size; maker_it++) {
        name owner = maker_it->miner;
        double r1 = maker_it->current_rate;
        auto pst_it = pst_acnts.find(owner.value);

        double m = get_dmc_rate(maker_it->benchmark_stake_rate);
        double sub_pst = (double)(1 - r1 / m) * get_real_asset(pst_it->amount);
        extended_asset liq_pst_asset_leftover = get_asset_by_amount<double, std::ceil>(sub_pst, pst_sym);
        auto origin_liq_pst_asset = liq_pst_asset_leftover;

        accounts acnts(get_self(), owner.value);
        auto account_idx = acnts.get_index<"byextendedas"_n>();
        auto account_it = account_idx.find(account::key(pst_sym));
        if (account_it != account_idx.end()) {
            extended_asset pst_sub = extended_asset(std::min(liq_pst_asset_leftover.quantity.amount, account_it->balance.quantity.amount), pst_sym);

            sub_balance(owner, pst_sub);
            liq_pst_asset_leftover.quantity.amount = std::max((liq_pst_asset_leftover - pst_sub).quantity.amount, 0ll);
        }

        bill_stats sst(get_self(), owner.value);
        for (auto bit = sst.begin(); bit != sst.end() && liq_pst_asset_leftover.quantity.amount > 0;) {
            extended_asset sub_pst;
            if (bit->unmatched <= liq_pst_asset_leftover) {
                sub_pst = bit->unmatched;
                liq_pst_asset_leftover -= bit->unmatched;
            } else {
                sub_pst = liq_pst_asset_leftover;
                liq_pst_asset_leftover.quantity.amount = 0;
            }

            name miner = bit->owner;
            uint64_t bill_id = bit->bill_id;
            uint64_t now_time_t = calbonus(miner, bill_id, _self);

            sst.modify(bit, get_self(), [&](auto& r) {
                r.unmatched -= sub_pst;
                r.updated_at = time_point_sec(now_time_t);
            });

            if (bit->unmatched.quantity.amount == 0)
                bit = sst.erase(bit);
            else
                bit++;

            SEND_INLINE_ACTION(*this, makerliqrec, { _self, "active"_n }, { miner, bill_id, sub_pst });
        }
        extended_asset sub_pst_asset = origin_liq_pst_asset - liq_pst_asset_leftover;
        double penalty_dmc = (double)(1 - r1 / m) * get_real_asset(maker_it->total_staked) * get_dmc_config("penaltyrate"_n, default_penalty_rate) / 100.0;
        extended_asset penalty_dmc_asset = get_asset_by_amount<double, std::ceil>(penalty_dmc, dmc_sym);
        if (sub_pst_asset.quantity.amount != 0 && penalty_dmc_asset.quantity.amount != 0) {
            liquidation_required.emplace_back(std::make_tuple(owner, sub_pst_asset, penalty_dmc_asset));
        }
    }

    for (const auto liq : liquidation_required) {
        name miner;
        extended_asset pst;
        extended_asset dmc;
        std::tie(miner, pst, dmc) = liq;
        auto pst_it = pst_acnts.find(miner.value);
        change_pst(miner, -pst);
        sub_stats(pst);
        auto iter = maker_tbl.find(miner.value);
        extended_asset new_staked = iter->total_staked - dmc;
        double new_rate = cal_current_rate(new_staked, miner);
        maker_tbl.modify(iter, get_self(), [&](auto& s) {
            s.total_staked = new_staked;
            s.current_rate = new_rate;
        });
        SEND_INLINE_ACTION(*this, makercharec, { _self, "active"_n }, { _self, miner, -dmc, MakerReceiptLiquidation });
        add_balance(system_account, dmc, eos_account);
        SEND_INLINE_ACTION(*this, liqrec, { _self, "active"_n }, { miner, pst, dmc });
    }
}

uint64_t token::calbonus(name owner, uint64_t bill_id, name ram_payer)
{
    bill_stats sst(get_self(), owner.value);
    auto ust_idx = sst.get_index<"byid"_n>();
    auto ust = ust_idx.find(bill_id);
    check(ust != ust_idx.end(), "no such record");

    dmc_makers maker_tbl(get_self(), get_self().value);
    const auto& iter = maker_tbl.get(owner.value, "no such pst maker");

    auto now_time = time_point_sec(current_time_point());
    uint64_t now_time_t = now_time.sec_since_epoch();
    uint64_t updated_at_t = ust->updated_at.sec_since_epoch();
    uint64_t bill_dmc_claims_interval = get_dmc_config("billinter"_n, default_bill_dmc_claims_interval);
    uint64_t max_dmc_claims_interval = ust->created_at.sec_since_epoch() + bill_dmc_claims_interval;

    now_time_t = now_time_t >= max_dmc_claims_interval ? max_dmc_claims_interval : now_time_t;

    if (updated_at_t <= max_dmc_claims_interval) {
        uint64_t duration = now_time_t - updated_at_t;
        check(duration <= now_time_t, "subtractive overflow"); // never happened

        extended_asset quantity = get_asset_by_amount<double, std::floor>(incentive_rate * get_dmc_config("bmrate"_n, default_benchmark_stake_rate) / 100.0 / default_bill_dmc_claims_interval, rsi_sym);
        quantity.quantity.amount *= duration;
        quantity.quantity.amount *= ust->unmatched.quantity.amount;
        if (quantity.quantity.amount != 0) {
            extended_asset dmc_quantity = get_dmc_by_vrsi(quantity);
            if (dmc_quantity.quantity.amount > 0) {
                maker_tbl.modify(iter, ram_payer, [&](auto& s) {
                    s.total_staked += dmc_quantity;
                });
                SEND_INLINE_ACTION(*this, incentiverec, {_self, "active"_n}, {owner, dmc_quantity, bill_id, 0, 0});
            }
        }
    }
    return now_time_t;
}

void token::setabostats(uint64_t stage, double user_rate, double foundation_rate, extended_asset total_release, time_point_sec start_at, time_point_sec end_at)
{
    require_auth(eos_account);
    check(stage >= 1 && stage <= 11, "invalid stage");
    check(user_rate <= 1 && user_rate >= 0, "invalid user_rate");
    check(foundation_rate + user_rate == 1, "invalid foundation_rate");
    check(start_at < end_at, "invalid time");
    check(total_release.get_extended_symbol() == dmc_sym, "invalid symbol");
    bool set_now = false;
    auto now_time = time_point_sec(current_time_point());
    if (now_time > start_at)
        set_now = true;

    abostats ast(get_self(), get_self().value);
    const auto& st = ast.find(stage);
    if (st != ast.end()) {
        ast.modify(st, get_self(), [&](auto& a) {
            a.user_rate = user_rate;
            a.foundation_rate = foundation_rate;
            a.total_release = total_release;
            a.start_at = start_at;
            a.end_at = end_at;
        });
    } else {
        ast.emplace(_self, [&](auto& a) {
            a.stage = stage;
            a.user_rate = user_rate;
            a.foundation_rate = foundation_rate;
            a.total_release = total_release;
            a.remaining_release = total_release;
            a.start_at = start_at;
            a.end_at = end_at;
            if (set_now){
                a.last_user_released_at = time_point_sec(current_time_point());
                a.last_foundation_released_at = time_point_sec(current_time_point());
            } else {
                a.last_user_released_at = start_at;
                a.last_foundation_released_at = start_at;
            }
        });
    }
}

void token::setdmcconfig(name key, uint64_t value)
{
    require_auth(eos_account);
    dmc_global dmc_global_tbl(get_self(), get_self().value);
    switch (key.value) {
    case ("claiminter"_n).value:
        check(value > 0, "invalid claims interval");
        break;
    default:
        break;
    }
    auto config_itr = dmc_global_tbl.find(key.value);
    if (config_itr == dmc_global_tbl.end()) {
        dmc_global_tbl.emplace(_self, [&](auto& conf) {
            conf.key = key;
            conf.value = value;
        });
    } else {
        dmc_global_tbl.modify(config_itr, get_self(), [&](auto& conf) {
            conf.value = value;
        });
    }
}

uint64_t token::get_dmc_config(name key, uint64_t default_value)
{
    dmc_global dmc_global_tbl(get_self(), get_self().value);
    auto dmc_global_iter = dmc_global_tbl.find(key.value);
    if (dmc_global_iter != dmc_global_tbl.end())
        return dmc_global_iter->value;
    return default_value;
}

double token::get_dmc_rate(uint64_t rate_value)
{
    avg_table atb(get_self(), get_self().value);
    auto aitr = atb.begin();
    double value = rate_value / 100.0;
    if (aitr == atb.end()) {
        auto avg_price = get_dmc_config("initalprice"_n, default_initial_price);
        return value * avg_price;
    }
    return value * aitr->avg;
}

void token::trace_price_history(double price, uint64_t bill_id)
{
    price_table ptb(get_self(), get_self().value);
    auto iter = ptb.get_index<"bytime"_n>();
    auto now_time = time_point_sec(current_time_point());
    auto rtime = now_time - price_fluncuation_interval;

    avg_table atb(get_self(), get_self().value);
    auto aitr = atb.begin();
    if (aitr == atb.end()) {
        aitr = atb.emplace(_self, [&](auto& a) {
            a.primary = 0;
            a.total = 0;
            a.count = 0;
            a.avg = 0;
        });
    }

    for (auto it = iter.begin(); it != iter.end();) {
        if (it->created_at < rtime) {
            it = iter.erase(it);
            atb.modify(aitr, _self, [&](auto& a) {
                a.total -= it->price;
                a.count -= 1;
            });
        } else {
            break;
        }
    }

    ptb.emplace(_self, [&](auto& p) {
        p.primary = ptb.available_primary_key();
        p.bill_id = bill_id;
        p.price = price;
        p.created_at = now_time;
    });

    atb.modify(aitr, _self, [&](auto& a) {
        a.total += price;
        a.count += 1;
        a.avg = (double)a.total / a.count;
    });
}
} // namespace eosio