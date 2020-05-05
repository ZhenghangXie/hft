#include <iostream>
#include <string>
#include <algorithm>
#include <vector>

#include "./strategy.h"

Strategy::Strategy(const libconfig::Setting & param_setting, std::unordered_map<std::string, std::vector<BaseStrategy*> >*ticker_strat_map, Sender<MarketSnapshot>* uisender, Sender<Order>* ordersender, HistoryWorker* hw, TimeController* tc, ContractWorker* cw, const std::string & mode, std::ofstream* exchange_file)
  : mode(mode),
    last_valid_mid(0.0),
    stop_loss_times(0),
    max_close_try(10),
    no_close_today(false),
    m_hw(hw),
    max_round(10000),
    close_round(0),
    sample_head(0),
    sample_tail(0),
    exchange_file(exchange_file) {
  m_tc = tc;
  m_cw = cw;
  if (FillStratConfig(param_setting)) {
    RunningSetup(ticker_strat_map, uisender, ordersender, mode);
  }
}

Strategy::~Strategy() {
}

void Strategy::RunningSetup(std::unordered_map<std::string, std::vector<BaseStrategy*> >*ticker_strat_map, Sender<MarketSnapshot>* uisender, Sender<Order>* ordersender, const std::string & mode) {
  ui_sender = uisender;
  order_sender = ordersender;
  (*ticker_strat_map)[main_ticker].emplace_back(this);
  (*ticker_strat_map)[hedge_ticker].emplace_back(this);
  (*ticker_strat_map)["positionend"].emplace_back(this);
  MarketSnapshot shot;
  shot_map[main_ticker] = shot;
  shot_map[hedge_ticker] = shot;
  avgcost_map[main_ticker] = 0.0;
  avgcost_map[hedge_ticker] = 0.0;
  if (mode == "test" || mode == "nexttest") {
    position_ready = true;
  }
}

bool Strategy::FillStratConfig(const libconfig::Setting& param_setting) {
  try {
    std::string unique_name = param_setting["unique_name"];
    const libconfig::Setting & contract_setting = m_cw->Lookup(unique_name);
    m_strat_name = unique_name;
    std::vector<std::string> v = m_hw->GetTicker(unique_name);
    if (v.size() < 2) {
      printf("no enough ticker for %s\n", unique_name.c_str());
      PrintVector(v);
      return false;
    }
    main_ticker = v[1];
    hedge_ticker = v[0];
    max_pos = param_setting["max_position"];
    min_train_sample = param_setting["min_train_samples"];
    double m_r = param_setting["min_range"];
    double m_p = param_setting["min_profit"];
    min_price_move = contract_setting["min_price_move"];
    printf("[%s, %s] mpv is %lf\n", main_ticker.c_str(), hedge_ticker.c_str(), min_price_move);
    min_profit = m_p * min_price_move;
    min_range = m_r * min_price_move;
    double add_margin = param_setting["add_margin"];
    increment = add_margin*min_price_move;
    double spread_threshold_int = param_setting["spread_threshold"];
    spread_threshold = spread_threshold_int*min_price_move;
    stop_loss_margin = param_setting["stop_loss_margin"];
    max_loss_times = param_setting["max_loss_times"];
    max_holding_sec = param_setting["max_holding_sec"];
    range_width = param_setting["range_width"];
    std::string con = GetCon(main_ticker);
    cancel_limit = contract_setting["cancel_limit"];
    max_round = param_setting["max_round"];
    split_num = param_setting["split_num"];
    if (param_setting.exists("no_close_today")) {
      no_close_today = param_setting["no_close_today"];
    }
    printf("[%s %s] try over!\n", main_ticker.c_str(), hedge_ticker.c_str());
  } catch(const libconfig::SettingNotFoundException &nfex) {
    printf("Setting '%s' is missing", nfex.getPath());
    exit(1);
  } catch(const libconfig::SettingTypeException &tex) {
    printf("Setting '%s' has the wrong type", tex.getPath());
    exit(1);
  } catch (const std::exception& ex) {
    printf("EXCEPTION: %s\n", ex.what());
    exit(1);
  }
  up_diff = 0.0;
  down_diff = 0.0;
  stop_loss_up_line = 0.0;
  stop_loss_down_line = 0.0;
  return true;
}

void Strategy::Stop() {
  CancelAll(main_ticker);
  ss = StrategyStatus::Stopped;
}

bool Strategy::Spread_Good() {
  return (current_spread > spread_threshold) ? false : true;
}

bool Strategy::IsAlign() {
  if (shot_map[main_ticker].time.tv_sec == shot_map[hedge_ticker].time.tv_sec && abs(shot_map[main_ticker].time.tv_usec-shot_map[hedge_ticker].time.tv_usec) < 100000) {
    return true;
  }
  return false;
}

OrderSide::Enum Strategy::OpenLogicSide() {
  double mid = GetPairMid();
  if (mid - current_spread/2 > up_diff) {
    printf("[%s %s]sell condition hit, as diff id %f\n",  main_ticker.c_str(), hedge_ticker.c_str(), mid);
    return OrderSide::Sell;
  } else if (mid + current_spread/2 < down_diff) {
    printf("[%s %s]buy condition hit, as diff id %f\n", main_ticker.c_str(), hedge_ticker.c_str(), mid);
    return OrderSide::Buy;
  } else {
    return OrderSide::Unknown;
  }
}

void Strategy::DoOperationAfterCancelled(Order* o) {
  printf("ticker %s cancel num %d!\n", o->ticker, cancel_map[o->ticker]);
  if (cancel_map[o->ticker] > cancel_limit) {
    printf("ticker %s hit cancel limit!\n", o->ticker);
    Stop();
  }
}

double Strategy::OrderPrice(const std::string & ticker, OrderSide::Enum side, bool control_price) {
  if (mode == "nexttest") {
    if (ticker == hedge_ticker) {
      return (side == OrderSide::Buy)?next_shot_map[ticker].asks[0]:next_shot_map[ticker].bids[0];
    } else if (ticker == main_ticker) {
      return (side == OrderSide::Buy)?shot_map[ticker].asks[0]:shot_map[ticker].bids[0];
    } else {
      printf("error ticker %s\n", ticker.c_str());
      return -1.0;
    }
  } else {
    if (ticker == hedge_ticker) {
      return (side == OrderSide::Buy)?shot_map[hedge_ticker].asks[0]:shot_map[hedge_ticker].bids[0];
    } else if (ticker == main_ticker) {
      return (side == OrderSide::Buy)?shot_map[main_ticker].asks[0]:shot_map[main_ticker].bids[0];
    } else {
      printf("error ticker %s\n", ticker.c_str());
      return -1.0;
    }
  }
}

std::tuple<double, double> Strategy::CalMeanStd(const std::vector<double> & v, int head, int num) {
  std::vector<double> cal_v(v.begin() + head, v.begin() + head + num);
  double mean = 0.0;
  double std = 0.0;
  for (auto i : cal_v) {
    mean += i;
  }
  mean /= num;
  for (auto i : cal_v) {
    std += (i-mean) * (i-mean);
  }
  std /= num;
  std = sqrt(std);
  return std::tie(mean, std);
}

void Strategy::CalParams() {
  if (sample_tail < min_train_sample) {
    printf("[%s %s]no enough mid data! tail is %d\n", main_ticker.c_str(), hedge_ticker.c_str(), sample_tail);
    exit(1);
  }
  param_v.clear();
  auto r = CalMeanStd(map_vector, sample_tail - min_train_sample, min_train_sample);
  double avg = std::get<0>(r);
  double std = std::get<1>(r);
  FeePoint main_point = m_cw->CalFeePoint(main_ticker, GetMid(main_ticker), 1, GetMid(main_ticker), 1, no_close_today);
  FeePoint hedge_point = m_cw->CalFeePoint(hedge_ticker, GetMid(hedge_ticker), 1, GetMid(hedge_ticker), 1, no_close_today);
  double round_fee_cost = main_point.open_fee_point + main_point.close_fee_point + hedge_point.open_fee_point + hedge_point.close_fee_point;
  double margin = std::max(range_width * std, min_range) + round_fee_cost;
  up_diff = avg + margin;
  down_diff = avg - margin;
  stop_loss_up_line = up_diff + stop_loss_margin * margin;
  stop_loss_down_line = down_diff - stop_loss_margin * margin;
  mean = avg;
  spread_threshold = margin - min_profit - round_fee_cost;
  printf("[%s %s]cal done,mean is %lf, std is %lf, parmeters: [%lf,%lf], spread_threshold is %lf, min_profit is %lf, up_loss=%lf, down_loss=%lf fee_point=%lf\n", main_ticker.c_str(), hedge_ticker.c_str(), avg, std, down_diff, up_diff, spread_threshold, min_profit, stop_loss_up_line, stop_loss_down_line, round_fee_cost);
  sample_head = sample_tail;
}

bool Strategy::HitMean() {
  double this_mid = GetPairMid();
  int pos = position_map[main_ticker];
  if (pos > 0 && this_mid - current_spread/2 >= mean) {  // buy position
    printf("[%s %s] mean is %lf, this_mid is %lf, current_spread is %lf, pos is %d\n", main_ticker.c_str(), hedge_ticker.c_str(), mean, this_mid, current_spread, pos);
    return true;
  } else if (pos < 0 && this_mid + current_spread/2 <= mean) {  // sell position
    printf("[%s %s] mean is %lf, this_mid is %lf, current_spread is %lf, pos is %d\n", main_ticker.c_str(), hedge_ticker.c_str(), mean, this_mid, current_spread, pos);
    return true;
  }
  return false;
}

void Strategy::ForceFlat() {
  printf("%ld [%s %s]this round hit stop_loss condition, pos:%d current_mid:%lf, current_spread:%lf stoplossline %lf-%lf forceflat\n", shot_map[hedge_ticker].time.tv_sec, main_ticker.c_str(), hedge_ticker.c_str(), position_map[main_ticker], GetPairMid(), current_spread, stop_loss_down_line, stop_loss_up_line);
  shot_map[main_ticker].Show(stdout);
  shot_map[hedge_ticker].Show(stdout);
  for (int i = 0; i < max_close_try; i++) {
    if (Close(true)) {
      break;
    }
    if (i == max_close_try - 1) {
      printf("[%s %s]try max_close times, cant close this order!\n", main_ticker.c_str(), hedge_ticker.c_str());
      PrintMap(order_map);
      order_map.clear();  // it's a temp solution, TODO
      Close();
    }
  }
}

void Strategy::RecordSlip(const std::string & ticker, OrderSide::Enum side, bool is_close) {
    double slip = (side == OrderSide::Buy)? shot_map[ticker].asks[0] - next_shot_map[ticker].asks[0] : next_shot_map[ticker].bids[0] - shot_map[ticker].bids[0];
  if (ticker == hedge_ticker) {
    printf("Slip%s hedge[%s] %s: %lf %lf ->> %lf %lf pnl:%lf\n", is_close ? " close" : " open", ticker.c_str(), OrderSide::ToString(side), shot_map[ticker].asks[0], shot_map[ticker].bids[0], next_shot_map[ticker].asks[0], next_shot_map[ticker].bids[0], slip);
  } else if (ticker == main_ticker) {
    printf("Slip%s main[%s] %s: %lf %lf ->> %lf %lf pnl:%lf\n", is_close ? " close" : " open", ticker.c_str(), OrderSide::ToString(side), shot_map[ticker].asks[0], shot_map[ticker].bids[0], next_shot_map[ticker].asks[0], next_shot_map[ticker].bids[0], slip);
  } else {
    printf("error ticker %s\n", ticker.c_str());
  }
}

bool Strategy::Close(bool force_flat) {
  int pos = position_map[main_ticker];
  if (pos == 0) {
    return true;
  }
  OrderSide::Enum close_side = pos > 0 ? OrderSide::Sell: OrderSide::Buy;
  printf("close using %s: pos is %d, diff is %lf\n", OrderSide::ToString(close_side), pos, GetPairMid());
  PrintMap(position_map);
  if (order_map.empty()) {
    PrintMap(avgcost_map);
    Order* o = NewOrder(main_ticker, close_side, abs(pos), false, false, force_flat ? "force_flat_close" : "close", no_close_today);  // close
    RecordSlip(main_ticker, o->side, true);
    o->Show(stdout);
    HandleTestOrder(o);
    if (mode == "real") {
    }
    return true;
  } else {
    printf("[%s %s]block order exsited! no close\n", main_ticker.c_str(), hedge_ticker.c_str());
    PrintMap(order_map);
    return false;
  }
}

double Strategy::GetPairMid() {
  return GetMid(main_ticker) - GetMid(hedge_ticker);
}

void Strategy::CloseLogic() {
  int pos = position_map[main_ticker];
  if (pos == 0) {
    return;
  }

  if (TimeUp()) {
    printf("[%s %s] holding time up, start from %ld, now is %ld, max_hold is %d close diff is %lf force to close position!\n", main_ticker.c_str(), hedge_ticker.c_str(), build_position_time, mode == "test" || mode == "nexttest" ? shot_map[main_ticker].time.tv_sec : m_tc->CurrentInt(), max_holding_sec, GetPairMid());
    ForceFlat();
    return;
  }

  if (HitMean()) {
    if (Close()) {
    }
    return;
  }
}

void Strategy::Flatting() {
  if (IsAlign()) {
    CloseLogic();
  }
}

void Strategy::Open(OrderSide::Enum side) {
  int pos = position_map[main_ticker];
  printf("[%s %s] open %s: pos is %d, diff is %lf\n", main_ticker.c_str(), hedge_ticker.c_str(), OrderSide::ToString(side), pos, GetPairMid());
  if (order_map.empty()) {  // no block order, can add open
    Order* o = NewOrder(main_ticker, side, 1, false, false, "", no_close_today);
    RecordSlip(main_ticker, o->side);
    o->Show(stdout);
    HandleTestOrder(o);
  } else {  // block order exsit, no open, possible reason: no enough margin
    printf("block order exsited! no open \n");
    PrintMap(order_map);
  }
}

bool Strategy::OpenLogic() {
  OrderSide::Enum side = OpenLogicSide();
  if (side == OrderSide::Unknown) {
    return false;
  }
  // do meet the logic
  int pos = position_map[main_ticker];
  if (abs(pos) == max_pos) {
    // hit max, still update bound
    // UpdateBound(side == OrderSide::Buy ? OrderSide::Sell : OrderSide::Buy);
    return false;
  }
  Open(side);
  return true;
}

void Strategy::Run() {
  if (IsAlign() && close_round < max_round) {
      if (!OpenLogic()) {
        CloseLogic();
      }
  } else {
  }
}

void Strategy::Init() {
  ticker_map[main_ticker] = true;
  ticker_map[hedge_ticker] = true;
  ticker_map["positionend"] = true;
}

void Strategy::DoOperationAfterUpdateData(const MarketSnapshot& shot) {
  if (IsAlign()) {
    double long_price = shot_map[main_ticker].asks[0] - shot_map[hedge_ticker].bids[0];
    double short_price = shot_map[main_ticker].bids[0] - shot_map[hedge_ticker].asks[0];
    long_vector.emplace_back(long_price);
    short_vector.emplace_back(short_price);
    sample_tail ++;
    if (sample_tail - sample_head == update_samples) {
      UpdateParams();
    }
  }
}

void Strategy::UpdatePos() {

}

void Strategy::HandleCommand(const Command& shot) {
  printf("received command!\n");
  shot.Show(stdout);
  if (abs(shot.vdouble[0]) > MIN_DOUBLE_DIFF) {
    up_diff = shot.vdouble[0];
    return;
  }
  if (abs(shot.vdouble[1]) > MIN_DOUBLE_DIFF) {
    down_diff = shot.vdouble[1];
    return;
  }
  if (abs(shot.vdouble[2]) > MIN_DOUBLE_DIFF) {
    stop_loss_up_line = shot.vdouble[2];
    return;
  }
  if (abs(shot.vdouble[3]) > MIN_DOUBLE_DIFF) {
    stop_loss_down_line = shot.vdouble[3];
    return;
  }
}

void Strategy::Train() {
}

void Strategy::Pause() {
}

void Strategy::Resume() {
  sample_head = sample_tail;
}

bool Strategy::Ready() {
  int num_sample = sample_tail - sample_head;
  if (position_ready && shot_map[main_ticker].IsGood() && shot_map[hedge_ticker].IsGood() && num_sample >= min_train_sample) {
    if (num_sample == min_train_sample) {
      // first cal params
      CalParams();
    }
    return true;
  }
  if (!position_ready) {
    printf("waiting position query finish!\n");
  }
  return false;
}

void Strategy::ModerateOrders(const std::string & ticker) {
  // just make sure the order filled
  if (mode == "real") {
    for (auto m:order_map) {
      Order* o = m.second;
      if (o->Valid()) {
        std::string ticker = o->ticker;
        MarketSnapshot shot = shot_map[ticker];
        double reasonable_price = (o->side == OrderSide::Buy ? shot.asks[0] : shot.bids[0]);
        bool is_price_move = (fabs(reasonable_price - o->price) >= min_price_move/2);
        if (!is_price_move) {
          continue;
        }
        if (ticker == main_ticker) {
          printf("[%s %s]Abandon this oppounity because main ticker price change %lf->%lf mpv=%lf\n", main_ticker.c_str(), hedge_ticker.c_str(), o->price, reasonable_price, min_price_move);
          CancelOrder(o);
        } else if (ticker == hedge_ticker) {
          printf("[%s %s]Slip point for :modify %s order %s: %lf->%lf mpv=%lf\n", main_ticker.c_str(), hedge_ticker.c_str(), OrderSide::ToString(o->side), o->order_ref, o->price, reasonable_price, min_price_move);
          ModOrder(o);
        } else {
          continue;
        }
      }
    }
  }
}

void Strategy::ClearPositionRecord() {
  avgcost_map.clear();
  position_map.clear();
}

void Strategy::Start() {
  if (!is_started) {
    ClearPositionRecord();
    is_started = true;
  }
  Run();
}

void Strategy::DoOperationAfterUpdatePos(Order* o, const ExchangeInfo& info) {
}

void Strategy::HandleTestOrder(Order* o) {
  if (mode != "test" && mode != "nexttest") {
    return;
  }
  ExchangeInfo info;
  info.shot_time = o->shot_time;
  info.type = InfoType::Filled;
  info.trade_size = o->size;
  info.trade_price = o->price;
  info.side = o->side;
  snprintf(info.order_ref, sizeof(info.order_ref), "%s", o->order_ref);
  snprintf(info.ticker, sizeof(info.ticker), "%s", o->ticker);
  snprintf(info.reason, sizeof(info.reason), "%s", "test");
  exchange_file->write(reinterpret_cast<char*>(&info), sizeof(info));
  exchange_file->flush();
  info.Show(stdout);
  UpdatePos(o, info);
  PrintMap(position_map);
  PrintMap(avgcost_map);
  DoOperationAfterFilled(o, info);
}


void Strategy::UpdateBuildPosTime() {
  int hedge_pos = position_map[hedge_ticker];
  if (hedge_pos == 0) {  // closed all position, reinitialize build_position_time
    build_position_time = MAX_UNIX_TIME;
  } else if (hedge_pos == 1) {  // position 0->1, record build_time
    build_position_time = m_tc->TimevalInt(last_shot.time);
  }
}

void Strategy::RecordPnl(Order* o, bool force_flat) {
  int pos = o->size;
  OrderSide::Enum pos_side = o->side == OrderSide::Sell ? OrderSide::Buy: OrderSide::Sell;
  OrderSide::Enum close_side = o->side;
  double hedge_price = pos > 0 ? shot_map[hedge_ticker].asks[0] : shot_map[hedge_ticker].bids[0];
  double this_round_pnl = m_cw->CalNetPnl(main_ticker, avgcost_map[main_ticker], abs(pos), o->price, abs(pos), close_side, no_close_today) + m_cw->CalNetPnl(hedge_ticker, avgcost_map[hedge_ticker], abs(pos), hedge_price, abs(pos), pos_side, no_close_today);
  std::string str = GetCon(main_ticker);
  std::string split_c = ",";
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%lf", this_round_pnl);
  str += split_c + buffer;
  for (auto i : param_v) {
    snprintf(buffer, sizeof(buffer), "%lf", i);
    str += split_c + buffer;
  }
  str += "\n";
  cout << "recordpnl," << str;
}

void Strategy::DoOperationAfterFilled(Order* o, const ExchangeInfo& info) {
  PrintMap(avgcost_map);
  o->Show(stdout);
  if (strcmp(o->ticker, main_ticker.c_str()) == 0) {
    // get hedged right now
    std::string a = o->tbd;
    if (a.find("close") != string::npos) {
      close_round++;
      RecordPnl(o);
      CalParams();
    } else {
    }
    Order* order = NewOrder(hedge_ticker, (o->side == OrderSide::Buy) ? OrderSide::Sell : OrderSide::Buy, info.trade_size, false, false, o->tbd, no_close_today);
    HandleTestOrder(order);
    order->Show(stdout);
  } else if (strcmp(o->ticker, hedge_ticker.c_str()) == 0) {
    UpdateBuildPosTime();
  } else {
    printf("o->ticker=%s, main:%s, hedge:%s\n", o->ticker, main_ticker.c_str(), hedge_ticker.c_str());
    SimpleHandle(322);
  }
}