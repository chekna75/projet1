#include "tradeorge.h"
#include <libxml/tree.h>
#include <libxml/HTMLparser.h>
#include <libxml++/libxml++.h>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/bind.hpp>
#include <boost/log/trivial.hpp>
#include <rapidjson/error/en.h>
#include <tradeorge.pb.h>
#include <thread>
#include <random>

using namespace boost::algorithm;
using namespace rapidjson;
using namespace currency;

namespace http = boost::beast::http;

tradeogre::tradeogre(deposit_callback depositCallbackFee,
               withdraw_callback withdrawCallbackFee,
               exchange_callback exchangeCallbackFee) :
        sync_api_handle_(std::make_shared<SyncHttps>(tradeogre_API_DOMAIN)), ping_pong_(120) {
    SetFeesCallback(depositCallbackFee, withdrawCallbackFee, exchangeCallbackFee);
    FetchFeesAndCurrencies();
}

void tradeogre::FetchFeesAndCurrencies() {
    FetchDepositWithdrawFees(false, [=](bool success) {
        if (!success) {
            throw "Cannot fetch deposit fees";
        }
        FetchTradingPairs(false, [=](bool success, trading_pairs_t pairs) {
            if (!success) {
                throw "Cannot fetch trading pairs";
            }
            FetchExchangeLimitsPrecision(false, [=](bool success, limit_quantities_t limits) {
                if (!success) {
                    throw "Cannot fetch limits";
                }
                FetchExchangeFees(false, pairs, limits, [=](bool success) {
                    if (!success) {
                        throw "Cannot fetch exchange fees";
                    }
                });
            });
        });
    });
}

void tradeogre::FetchFees(boost::asio::deadline_timer *timer, const boost::system::error_code &err) {
    if (err) {
        boost::asio::detail::throw_error(err, "tradeogre::FetchFees");
    }

    auto end = [=](bool success) {
        BOOST_LOG_TRIVIAL(info) << "[tradeogre] End fetching fees, res = " << (success ? "success" : "failed");
        timer->expires_from_now(boost::posix_time::seconds(FETCH_FEES_PERIOD));
        timer->async_wait(std::bind(&tradeogre::FetchFees, this, timer, std::placeholders::_1));
    };

    BOOST_LOG_TRIVIAL(info) << "[tradeogre] Start fetching fees";
    FetchDepositWithdrawFees(true, [=](bool success) {
        if (!success) {
            end(success);
        } else {
            FetchTradingPairs(true, [=](bool success, trading_pairs_t pairs) {
                if (!success) {
                    end(success);
                } else {
                    FetchExchangeLimitsPrecision(true, [=](bool success, limit_quantities_t limits) {
                        if (!success) {
                            end(success);
                        } else {
                            FetchExchangeFees(true, pairs, limits, [=](bool success) {
                                end(success);
                            });
                        }
                    });
                }
            });
        }
    });
}


void tradeogre::FetchDepositWithdrawFees(bool async, std::function<void(bool)> callback) {
    feesFlow.HttpGet(sync_api_handle_, async_api_handle_, async, callback, "/info/coinInfo", std::bind(&tradeogre::ParseDepositWithdrawFees, this, std::placeholders::_1));
}

void tradeogre::FetchExchangeLimitsPrecision(bool async, std::function<void(bool, limit_quantities_t limits)> callback) {
    std::map<std::string, std::string> headers;
    headers.insert(std::make_pair("Api-Key", tradeogreN_API_KEY));
    headers.insert(std::make_pair("Sign", tradeogre_SIGN_KEY));

    feesFlow.HttpGet(sync_api_handle_, async_api_handle_, async,
                     [this, callback](bool success) {
                         if (!success) {
                             callback(success, {});
                         }
                     }, "/exchange/restrictions",
                     [this, callback](std::string_view content) {
                         auto limits = ParseExchangeLimitsPrecision(content);
                         callback(true, limits);
                     },
                     boost::posix_time::milliseconds(INTERVAL_DURATION),
                     boost::posix_time::seconds(TIMEOUT_DURATION),
                     10,
                     headers);
}

void tradeogre::FetchExchangeFees(bool async, trading_pairs_t pairs, limit_quantities_t limits, std::function<void(bool)> callback) {
    feesFlow.HttpGet(sync_api_handle_, async_api_handle_, async, callback, "/exchange/commission", std::bind(
            &tradeogre::ParseExchangeFees, this, std::placeholders::_1, pairs, limits));
}

void tradeogre::FetchTradingPairs(bool async, std::function<void(bool, trading_pairs_t pairs)> callback) {
    feesFlow.HttpGet(sync_api_handle_, async_api_handle_, async, [this, callback](bool success) {
                         if (!success) {
                             callback(success, {});
                         }
                     }, "/exchange/ticker",
                [this, callback](std::string_view content) {
                    auto pairs = ParseTradingPairs(content);
                    callback(true, pairs);
                });
}

trading_pairs_t tradeogre::GetTradingPairs() {
    http::response<http::dynamic_body> json;
    std::string content;
    json = sync_api_handle_->get("/exchange/ticker");
    content = buffers_to_string(json.body().data());

    return ParseTradingPairs(content);
}

void tradeogre::ParseDepositWithdrawFees(std::string_view content) {
    Document document;
    document.Parse(content.data());

    if (document.HasParseError()) {
        throw std::runtime_error(std::string("JsonParsing Error: ") + GetParseError_En(document.GetParseError()));
    }

    if (!document.HasMember("success") || !document["success"].IsBool()) {
        throw std::runtime_error("Expected data member, not found");
    }

    if (!document["success"].GetBool()) {
        throw std::runtime_error(std::string("tradeogre request failed ").append(content));
    }

    if (!document.HasMember("info") || !document["info"].IsArray()) {
        throw std::runtime_error(std::string("tradeogre no info ").append(content));
    }

    WithdrawFeesMap withdraw_fees;
    DepositFeesMap deposit_fees;
    CurrenciesSet currencies;

    for (const auto &coin: document["info"].GetArray()) {
        if (!coin.HasMember("symbol")) {
            throw std::runtime_error("Expecting symbol, not found");
        }

        auto v = currency::FromStr(coin["symbol"].GetString());
        if (v.has_value()) {
            currencies.insert(*v);
            if (coin.HasMember("walletStatus") && coin["walletStatus"].IsString() && coin["walletStatus"].GetString() != std::string_view("down")) {

                if (!coin.HasMember("withdrawFee") || !coin.HasMember("minWithdrawAmount")) {
                    throw std::runtime_error("Missing withdrawFee or minWithdrawAmount");
                }

                auto fee = Number::FromDouble(coin["withdrawFee"].GetDouble());
                std::string amount_str = coin["minWithdrawAmount"].IsNumber() ? "" : coin["minWithdrawAmount"].GetString();
                trim(amount_str);
                auto amount = coin["minWithdrawAmount"].IsNumber() ? Number::FromDouble(coin["minWithdrawAmount"].GetDouble()) : Number::FromStr(amount_str);

                if (!fee.has_value() || !amount.has_value()) {
                    throw std::runtime_error("Unable to parse fees");
                }
                if (coin["walletStatus"].GetString() != std::string_view("closed_cashout")) {
                    withdraw_fees.insert(
                            std::make_pair(*v,
                                           Fee(Currency(*fee, *v), Currency(Number(0), *v), Currency(*amount, *v))));
                }

                if (!coin.HasMember("minDepositAmount")) {
                    throw std::runtime_error("Missing withdrawFee or minWithdrawAmount");
                }
                auto minAmount = Number::FromDouble(coin["minDepositAmount"].GetDouble());
                if (!minAmount.has_value()) {
                    throw std::runtime_error("Unable to parse fees");
                }
                if (coin["walletStatus"].GetString() != std::string_view("closed_cashin") && coin["walletStatus"].GetString() != std::string_view("delisted")) {
                    deposit_fees.insert(
                            std::make_pair(*v, Fee(Currency(Number(0), *v), Currency(Number(0), *v),
                                                   Currency(*minAmount, *v))));
                }
            }
        }
    }

    SetSupportedCurrencies(currencies);

    if (withdraw_fees.size() > 0) {
        SetWithdrawFees(withdraw_fees);
    }

    if (deposit_fees.size() > 0) {
        SetDepositFees(deposit_fees);
    }
}

limit_quantities_t tradeogre::ParseExchangeLimitsPrecision(std::string_view content) {
    Document document;
    document.Parse(content.data());

    if (document.HasParseError()) {
        throw std::runtime_error(std::string("JsonParsing Error: ") + GetParseError_En(document.GetParseError()));
    }

    if (!document.HasMember("success") || !document["success"].IsBool()) {
        throw std::runtime_error("Expected data member, not found");
    }

    if (!document["success"].GetBool()) {
        throw std::runtime_error(std::string("tradeogre request failed ").append(content));
    }

    if (!document.HasMember("restrictions") || !document["restrictions"].IsArray()) {
        throw std::runtime_error(std::string("tradeogre no info ").append(content));
    }

    PricePrecisionsMap precisions;
    limit_quantities_t min_limit_quantity;

    for (const auto &coin: document["restrictions"].GetArray()) {
        if (!coin.HasMember("currencyPair")) {
            throw std::runtime_error("Expecting currencyPair, not found");
        }

        std::vector<std::string> parts;
        std::string pair = coin["currencyPair"].GetString();
        split(parts, pair, is_any_of("/"));

        auto from = currency::FromStr(parts[0]);
        auto to = currency::FromStr(parts[1]);

        if (!from.has_value() || !to.has_value()) {
            continue;
        }

        if (!coin.HasMember("priceScale")) {
            throw std::runtime_error("Missing priceScale");
        }
        const auto precision = coin["priceScale"].GetInt64();
        if (precision > NUMBER_DECIMALS || precision < 0) {
            throw std::runtime_error("tradePrecision should be between 0 and NUMBER_DECIMALS included");
        }
        auto prec = Number(Number::POW_10[NUMBER_DECIMALS - precision]);
        precisions.insert(std::make_pair(std::make_pair(*from, *to), Currency(prec, *from)));

        if (!coin.HasMember("minLimitQuantity")) {
            throw std::runtime_error("Missing minLimitQuantity");
        }
        auto limit = Number::FromDouble(coin["minLimitQuantity"].GetDouble());
        if (!limit.has_value()) {
            throw std::runtime_error("Cannot convert minLimitQuantity");
        }
        min_limit_quantity.insert(std::make_pair(std::make_pair(*from, *to), *limit));
    }

    SetPricePrecisions(precisions);
    return min_limit_quantity;
}

trading_pairs_t tradeogre::ParseTradingPairs(std::string_view content) {
    Document document;
    document.Parse(content.data());

    if (document.HasParseError()) {
        throw std::runtime_error(std::string("JsonParsing Error: ") + GetParseError_En(document.GetParseError()));
    }

    if (!document.IsArray()) {
        throw std::runtime_error(std::string("tradeogre no info ").append(content));
    }

    trading_pairs_t pairs;

    for (const auto &coin: document.GetArray()) {
        if (!coin.HasMember("symbol")) {
            throw std::runtime_error("Expecting symbol, not found");
        }

        std::vector<std::string> parts;
        std::string sym = coin["symbol"].GetString();
        split(parts, sym, is_any_of("/"));

        auto from = currency::FromStr(parts[0]);
        auto to = currency::FromStr(parts[1]);

        if (!from.has_value() || !to.has_value()) {
            continue;
        }

        // Ignore pairs where min_bid and min_ask are zero, and volume is 0
        if (
                coin["max_bid"].GetDouble() == 0 &&
                        coin["min_ask"].GetDouble() == 0 &&
                        coin["best_bid"].GetDouble() == 0 &&
                        coin["best_ask"].GetDouble() == 0 &&
                        coin["volume"].GetDouble() == 0
                ) {
            continue;
        }

        pairs.push_back(std::make_pair(*from, *to));
    }

    return pairs;
}

void tradeogre::ParseExchangeFees(std::string_view content, trading_pairs_t pairs, limit_quantities_t limits) {
    Document document;
    document.Parse(content.data());

    if (document.HasParseError()) {
        throw std::runtime_error(std::string("JsonParsing Error: ") + GetParseError_En(document.GetParseError()));
    }

    if (!document.HasMember("success") || !document["success"].IsBool()) {
        throw std::runtime_error("Expected data member, not found");
    }

    if (!document["success"].GetBool()) {
        throw std::runtime_error(std::string("tradeogre request failed ").append(content));
    }

    if (!document.HasMember("fee") || !document["fee"].IsString()) {
        throw std::runtime_error(std::string("tradeogre no fee ").append(content));
    }

    auto fee = Number::FromStr(document["fee"].GetString());
    if (!fee.has_value()) {
        throw std::runtime_error(std::string("tradeogre fee parsing error ").append(content));
    }

    ExchangeFeesMap fees;
    for (const auto &c: pairs) {
        auto it = limits.find(c);
        if (it == limits.end()) {
            throw std::runtime_error(std::string("tradeogre missing min limits for pairs ").append(currency::ToStr(c.first)).append(currency::ToStr(c.second)));
        }

        auto fee_from = Fee(Currency(Number(0), c.second), Currency(*fee, c.second), Currency(it->second, c.second));
        auto fee_to = Fee(Currency(Number(0), c.first), Currency(*fee, c.first), Currency(Number(0), c.first));
        fees.insert(std::make_pair(c, fee_from));
        fees.insert(std::make_pair(std::make_pair(c.second, c.first), fee_to));
    }
    SetExchangeFees(fees);
}

#if FETCH_ORDERS
static void PushToOrders(const CurrencySymbol &from_c, const CurrencySymbol &to_c,
                         const google::protobuf::RepeatedPtrField<protobuf::ws::OrderBookEvent> &orders,
                         std::vector<Order> &orders_sell,
                         std::vector<Order> &orders_buy) {
    for (auto i = 0; i < orders.size(); i++) {
        const auto &order = orders.Get(i);
        const auto dir = order.order_type() == protobuf::ws::OrderBookEvent_OrderType::OrderBookEvent_OrderType_BID ? Order::Direction::BUY : Order::Direction::SELL;

        auto price = Number::FromStr(order.price());
        auto amount = Number::FromStr(order.quantity());

        if (!price.has_value() || !amount.has_value()) {
            continue;
        }

        if (dir == Order::Direction::BUY) {
            orders_buy.push_back(Order(from_c, to_c, Currency(*price, to_c), Currency(*amount, from_c), dir));
        } else {
            orders_sell.push_back(Order(from_c, to_c, Currency(*price, to_c), Currency(*amount, from_c), dir));
        }
    }
}

#endif

void tradeogre::SetupOrderBook(boost::asio::io_service &io,
                              const std::vector<std::pair<currency::CurrencySymbol, currency::CurrencySymbol>> &currencies,
                              std::function<void(std::vector<OrderBook*>)> callbackFull,
                              std::function<void(Order, std::string, Order::Action)> callbackDiff) {
#if FETCH_ORDERS
    ws_ = std::make_shared<WebSocket>(io);
    ws_->SetBinary(true);
    ws_->Run(tradeogre_WS_API_DOMAIN, "/ws/beta2", "", "https",
             [this, currencies, callbackFull, callbackDiff](std::string message, unsigned int ping_time) {

                 ping_pong_.RegisterPong(ping_time);
                 ping_pong_.RegisterPing(ping_time);

                 // Ping
                 if (message.size() == 0) {
                     return;
                 }

                 protobuf::ws::WsResponse response;
                 response.ParseFromString(message);
                 std::string currency_pairs;
                 google::protobuf::RepeatedPtrField<protobuf::ws::OrderBookEvent> orders;

                 if (response.meta().response_type() == protobuf::ws::WsResponseMetaData::ORDER_BOOK_CHANNEL_SUBSCRIBED) {
                     // Full
                     protobuf::ws::OrderBookChannelSubscribedResponse ocb;
                     ocb.ParseFromString(response.msg());

                     currency_pairs = ocb.currency_pair();
                     orders = ocb.data();
                 } else if (response.meta().response_type() == protobuf::ws::WsResponseMetaData::ORDER_BOOK_NOTIFY) {
                     // Diff
                     protobuf::ws::OrderBookNotification ocb;
                     ocb.ParseFromString(response.msg());

                     currency_pairs = ocb.currency_pair();
                     orders = ocb.data();
                 } else if (response.meta().response_type() == protobuf::ws::WsResponseMetaData::ERROR) {
                     protobuf::ws::ErrorResponse er;
                     er.ParseFromString(response.msg());

                     BOOST_LOG_TRIVIAL(error) << "[tradeogre] Error (" << er.code() << ") " << er.message();
                     return;
                 } else {
                     BOOST_LOG_TRIVIAL(error) << "[tradeogre] Unknown message type " << response.meta().response_type();
                     return;
                 }

                 std::vector<std::string> parts;
                 split(parts, currency_pairs, is_any_of("/"));

                 auto from = currency::FromStr(parts[0]);
                 auto to = currency::FromStr(parts[1]);

                 if (!from.has_value() || !to.has_value()) {
                     BOOST_LOG_TRIVIAL(error) << "[tradeogre] Unknown cur " << currency_pairs;
                     return;
                 }

                 std::vector<Order> orders_sell;
                 std::vector<Order> orders_buy;

                 PushToOrders(*from, *to, orders, orders_sell, orders_buy);

                 if (response.meta().response_type() == protobuf::ws::WsResponseMetaData::ORDER_BOOK_CHANNEL_SUBSCRIBED) {

                     auto ob_sell = new OrderBook(this, *from, *to, orders_sell, Order::Direction::SELL);
                     auto ob_buy = new OrderBook(this, *from, *to, orders_buy, Order::Direction::BUY);

                     ws_->GetIoService().post(
                             boost::bind(callbackFull, std::vector<OrderBook*>{ob_buy, ob_sell}));
                 } else if (response.meta().response_type() == protobuf::ws::WsResponseMetaData::ORDER_BOOK_NOTIFY) {
                     for (const auto &c: orders_sell) {
                         ws_->GetIoService().post(
                                 boost::bind(callbackDiff, c, "tradeogre", Order::Action::UPDATE_OR_DELETE));
                     }
                     for (const auto &c: orders_buy) {
                         ws_->GetIoService().post(
                                 boost::bind(callbackDiff, c, "tradeogre", Order::Action::UPDATE_OR_DELETE));
                     }
                 }
             }, [this,currencies](){
                // Subscribe to all channels
                for (const auto &c: currencies) {
                    protobuf::ws::SubscribeOrderBookChannelRequest record;
                    record.set_currency_pair(std::string(currency::ToStr(c.first)) + "/" + currency::ToStr(c.second));

                    std::string s;
                    record.SerializeToString(&s);

                    protobuf::ws::WsRequest wsRecord;
                    wsRecord.mutable_meta()->set_request_type(protobuf::ws::WsRequestMetaData::WsRequestMsgType::WsRequestMetaData_WsRequestMsgType_SUBSCRIBE_ORDER_BOOK);
                    wsRecord.set_msg(s);

                    std::string r;
                    wsRecord.SerializeToString(&r);

                    ws_->WriteMessage(r);
                }
            }, [this](unsigned int ping_time) {
                if (ping_pong_.ShouldReconnect(ping_time) || ping_time % 300 == 0) {
                    BOOST_LOG_TRIVIAL(warning) << "[tradeogre] No messages from tradeogre";
                    Reconnect(currency::CurrencySymbol::BTC, currency::CurrencySymbol::BTC);
                }
            }, cookies_, agent_);
#endif

    // Fetch fees continuously
    assert(async_api_handle_ == nullptr);
    async_api_handle_ = AsyncHttps::create(io, cookies_, agent_);
    async_api_handle_->Run(tradeogre_API_DOMAIN, [=]() {
        auto *timer = new boost::asio::deadline_timer(async_api_handle_->GetIoService(), boost::posix_time::seconds(FETCH_FEES_PERIOD));
        timer->async_wait(std::bind(&tradeogre::FetchFees, this, timer, std::placeholders::_1));
    });
}


void tradeogre::Reconnect(const currency::CurrencySymbol &from, const currency::CurrencySymbol &to) {
    ws_->ResetWebsocket(agent_, cookies_);
}

void tradeogre::Print(std::ostream &os) const {
    os << "tradeogre";
}
