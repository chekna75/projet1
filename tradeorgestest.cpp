//
// Created by hugo on 2/22/18.
//


#include <tests/CurrencyMacro.h>
#include <libtrading/exchange/tradeogre.h>

/*!
 * Instantiation tests the instantiation of the class
 */
TEST(tradeogreTest, Instantiation) {
    EXPECT_NO_THROW(tradeogre::create<tradeogre>());
}

/*!
 * Fees tests the code fetching the fees.
 * A fee equals to std::nullopt means that the exchange has the symbol disabled (or not supported).
 */
TEST(tradeogreTest, Fees) {
    auto co = tradeogre::create<tradeogre>();
    EXPECT_EQ(co->GetDepositFee(CurrencySymbol::BTC), Fee(ZB, ZB, ZB));
    EXPECT_EQ(co->GetDepositFee(CurrencySymbol::ONT), std::nullopt);
    EXPECT_EQ(*co->GetWithdrawFee(CurrencySymbol::BTC), Fee(CB(50000), ZB, CB(200000), ZB));
    EXPECT_EQ(*co->GetFinalWithdrawFee(CurrencySymbol::BTC), Fee(CB(50000), ZB, CB(200000), ZB));
    EXPECT_EQ(*co->GetWithdrawFee(CurrencySymbol::ETH), Fee(CE(1000000), ZE, CE(1000000), ZE));
    EXPECT_EQ(*co->GetFinalWithdrawFee(CurrencySymbol::ETH), Fee(CE(1000000), ZE, CE(1000000), ZE));

    EXPECT_EQ(*co->GetExchangeFee(CurrencySymbol::BTC, CurrencySymbol::ETH), Fee(ZE, CE(180000), ZE, ZE));
    EXPECT_EQ(*co->GetExchangeFee(CurrencySymbol::ETH, CurrencySymbol::BTC), Fee(ZB, CB(180000), CB(340000), ZB));
}

/*!
 * Precision tests the code fetching the precision on price.
 * Note that the precision is only one way (the way the pairs of traded symbols is defined).
 */
TEST(tradeogreTest, Precision) {
    auto co = tradeogre::create<tradeogre>();
    EXPECT_EQ(co->GetPricePrecision(CurrencySymbol::BCH, CurrencySymbol::BTC), Currency(Number(10000), CurrencySymbol::BCH));
    EXPECT_EQ(co->GetPricePrecision(CurrencySymbol::BTC, CurrencySymbol::BCH), std::nullopt);
}

/*!
 * GetTradingPairs tests the code fetching the traded pairs (with a few pairs).
 */
TEST(tradeogreTest, GetTradingPairs) {
    auto co = tradeogre::create<tradeogre>();
    auto v = co->GetTradingPairs();

    EXPECT_NE(std::find(v.begin(), v.end(), std::make_pair(CurrencySymbol::ETH, CurrencySymbol::BTC)), v.end());
    EXPECT_EQ(std::find(v.begin(), v.end(), std::make_pair(CurrencySymbol::CAT, CurrencySymbol::BTC)), v.end());
}
