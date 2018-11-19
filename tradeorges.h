#ifndef TRADING_TRADEORGE_H
#define TRADING_TRADEORGE_H


#include <vector>
#include <boost/asio/io_service.hpp>
#include <map>

#include "../order/Order.h"
#include "../order/OrderBook.h"
#include "../network/SyncHttps.h"
#include "../network/AsyncHttps.h"
#include "../network/HttpFlow.h"
#include "../network/WebSocket.h"
#include "../utils/PingPong.h"
#include "Exchange.h"

typedef std::map<std::pair<CurrencySymbol, CurrencySymbol>, Number> limit_quantities_t;

static const char *const tradeogre_API_DOMAIN = "tradeogre.com/api/v1";
static const char *const tradeogre_WS_API_DOMAIN = "ws.tradeogre.com/api/v1";
static const char *const tradeogre_API_KEY = "bc3c68818bae94045f3f160966c8ec5c";
static const char *const tradeogre_SECRET_KEY = "2e0fbb807ea1390c030db2e345a712c6";



class tradeogre : public Exchange {
    friend class Exchange;

private:
    void FetchFeesAndCurrencies();
    void Print(std::ostream &os) const;

    tradeogre(deposit_callback depositCallbackFee,
    withdraw_callback withdrawCallbackFee,
            exchange_callback exchangeCallbackFee);

    std::shared_ptr<WebSocket> ws_;

public:
    void SetupOrderBook(boost::asio::io_service &io,
                        const trading_pairs_t &currencies,
                        std::function<void(std::vector<OrderBook*>)> callbackFull,
                        std::function<void(Order, std::string, Order::Action)> callbackDiff);

    std::string_view GetName() const {
        return "tradeogre";
    }

    ExchangeType GetType() const {
        return ExchangeType::tradeogre;
    }

    trading_pairs_t GetTradingPairs();
    void Reconnect(const currency::CurrencySymbol &from, const currency::CurrencySymbol &to);
    HttpFlow feesFlow;

    std::shared_ptr<SyncHttps> sync_api_handle_;
    std::shared_ptr<AsyncHttps> async_api_handle_;
    std::map<std::string, std::string> cookies_;
    std::string agent_;
    PingPong ping_pong_;

    void FetchExchangeFees(bool async, trading_pairs_t pairs, limit_quantities_t limits, std::function<void(bool)> callback);
    void ParseExchangeFees(std::string_view text, trading_pairs_t pairs, limit_quantities_t limits);
    void ParseDepositWithdrawFees(std::string_view text);
    void FetchDepositWithdrawFees(bool async, std::function<void(bool)> callback);
    void FetchFees(boost::asio::deadline_timer *timer, const boost::system::error_code &err);
    void FetchExchangeLimitsPrecision(bool async, std::function<void(bool, limit_quantities_t)> callback);
    limit_quantities_t ParseExchangeLimitsPrecision(std::string_view content);
    void FetchTradingPairs(bool async, std::function<void(bool, trading_pairs_t pairs)> callback);
    trading_pairs_t ParseTradingPairs(std::string_view text);
};


#endif //TRADING_TRADEORGE_H
