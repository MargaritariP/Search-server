#pragma once
#include "document.h"
#include "paginator.h"
#include "string_processing.h"
#include "concurrent_map.h"
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>
#include <execution>

const int MAX_RESULT_DOCUMENT_COUNT = 5;

class SearchServer {
public:
    SearchServer(const std::string& stop_words_text)
        : SearchServer(SplitIntoWords(stop_words_text))
    {
    }

    SearchServer(std::string_view stop_words_text)
        : SearchServer(SplitIntoWordsView(stop_words_text))
    {
    }
    template <typename StringContainer>
    explicit SearchServer(const StringContainer& stop_words);
    void AddDocument(int document_id, std::string_view document, DocumentStatus status, const std::vector<int>& ratings);
    template <typename ExecutionPolicy, typename DocumentPredicate>
    std::vector<Document> FindTopDocuments(const ExecutionPolicy& policy, std::string_view raw_query, DocumentPredicate document_predicate) const;
    template <typename DocumentPredicate>
    std::vector<Document> FindTopDocuments(std::string_view raw_query, DocumentPredicate document_predicate) const;
    std::vector<Document> FindTopDocuments(std::string_view raw_query, DocumentStatus status) const;
    std::vector<Document> FindTopDocuments(std::string_view raw_query) const;
    template <typename ExecutionPolicy>
    std::vector<Document> FindTopDocuments(const ExecutionPolicy& policy, std::string_view raw_query, DocumentStatus status) const;
    template <typename ExecutionPolicy>
    std::vector<Document> FindTopDocuments(const ExecutionPolicy& policy, std::string_view raw_query) const;
    
    int GetDocumentCount() const;
    std::set<int>::const_iterator begin() const;
    std::set<int>::const_iterator end() const;
    const std::map<std::string_view, double>& GetWordFrequencies(int document_id) const;
    std::tuple<std::vector<std::string_view>, DocumentStatus> MatchDocument(std::string_view raw_query, int document_id) const;
    std::tuple<std::vector<std::string_view>, DocumentStatus> MatchDocument(std::execution::sequenced_policy, std::string_view raw_query, int document_id) const;
    std::tuple<std::vector<std::string_view>, DocumentStatus> MatchDocument(std::execution::parallel_policy, std::string_view raw_query, int document_id) const;
    void RemoveDocument(int document_id);
    void RemoveDocument(std::execution::sequenced_policy, int document_id);
    void RemoveDocument(std::execution::parallel_policy, int document_id);

private:
    struct DocumentData {
        int rating;
        DocumentStatus status;
        std::string text;
    };
    const std::set<std::string, std::less<>> stop_words_;
    std::map<int, std::map<std::string_view, double>> doc_to_word_freq;
    std::map<std::string_view, std::map<int, double>> word_to_document_freqs_;
    std::map<int, DocumentData> documents_;
    std::set<int> document_ids_;

    bool IsStopWord(std::string_view word) const;
    static bool IsValidWord(std::string_view word);
    std::vector<std::string_view> SplitIntoWordsNoStop(std::string_view text) const;
    static int ComputeAverageRating(const std::vector<int>& ratings);

    struct QueryWord {
        std::string_view data;
        bool is_minus;
        bool is_stop;
    };
    struct Query {
        std::vector<std::string_view> plus_words;
        std::vector<std::string_view> minus_words;
    };

    QueryWord ParseQueryWord(std::string_view text) const;
    Query ParseQuery(std::string_view text, bool is_sorted) const;
    double ComputeWordInverseDocumentFreq(std::string_view word) const;
    template <typename DocumentPredicate>
    std::vector<Document> FindAllDocuments(const Query& query,
        DocumentPredicate document_predicate) const;
    template <typename DocumentPredicate>
    std::vector<Document> FindAllDocuments(std::execution::sequenced_policy, const Query& query,
        DocumentPredicate document_predicate) const;
    template <typename DocumentPredicate>
    std::vector<Document> FindAllDocuments(std::execution::parallel_policy, const Query& query,
        DocumentPredicate document_predicate) const;
   
};

template <typename StringContainer>
SearchServer::SearchServer(const StringContainer& stop_words)
    : stop_words_(MakeUniqueNonEmptyStrings(stop_words))
{
    if (!all_of(stop_words_.begin(), stop_words_.end(), IsValidWord)) {
        throw std::invalid_argument("Some of stop words are invalid"s);
    }
}
template <typename DocumentPredicate>
std::vector<Document> SearchServer::FindTopDocuments(std::string_view raw_query, DocumentPredicate document_predicate) const {
    return FindTopDocuments(std::execution::seq, raw_query, document_predicate);
}
template <typename ExecutionPolicy, typename DocumentPredicate>
std::vector<Document> SearchServer::FindTopDocuments(const ExecutionPolicy& policy, std::string_view raw_query, DocumentPredicate document_predicate) const {
    const auto query = SearchServer::ParseQuery(raw_query, false);
    auto matched_documents = SearchServer::FindAllDocuments(policy, query, document_predicate);

    std::sort(policy, matched_documents.begin(), matched_documents.end(),
        [](const Document& lhs, const Document& rhs) {
            return lhs.relevance > rhs.relevance
                || (std::abs(lhs.relevance - rhs.relevance) < std::numeric_limits<double>::epsilon() && lhs.rating > rhs.rating);
        });

    if (matched_documents.size() > MAX_RESULT_DOCUMENT_COUNT) {
        matched_documents.resize(MAX_RESULT_DOCUMENT_COUNT);
    }
    return matched_documents;
}
template <typename ExecutionPolicy>
vector<Document> SearchServer::FindTopDocuments(const ExecutionPolicy& policy, string_view raw_query, DocumentStatus status) const {
    return FindTopDocuments(policy, raw_query,
        [&status](int document_id, DocumentStatus document_status, int rating) {
            return document_status == status;
        });
}
template <typename ExecutionPolicy>
vector<Document> SearchServer::FindTopDocuments(const ExecutionPolicy& policy, string_view raw_query) const {
    return FindTopDocuments(policy, raw_query, DocumentStatus::ACTUAL);
}
template <typename DocumentPredicate>
std::vector<Document> SearchServer::FindAllDocuments(std::execution::sequenced_policy, const Query& query,
    DocumentPredicate document_predicate) const {
   std::map<int, double> document_to_relevance;
    for (auto word : query.plus_words) {
        if (word_to_document_freqs_.count(word) == 0) {
            continue;
        }
        const double inverse_document_freq = SearchServer::ComputeWordInverseDocumentFreq(word);
        for (const auto [document_id, term_freq] : word_to_document_freqs_.at(word)) {
            const auto& document_data = documents_.at(document_id);
            if (document_predicate(document_id, document_data.status, document_data.rating)) {
                document_to_relevance[document_id] += term_freq * inverse_document_freq;
            }
        }
    }
    for (auto word : query.minus_words) {
        if (word_to_document_freqs_.count(word) == 0) {
            continue;
        }
        for (const auto [document_id, _] : word_to_document_freqs_.at(word)) {
            document_to_relevance.erase(document_id);
        }
    }

    vector<Document> matched_documents;
    for (const auto [document_id, relevance] : document_to_relevance) {
        matched_documents.push_back(
            { document_id, relevance, documents_.at(document_id).rating });
    }
    return matched_documents;
}
template <typename DocumentPredicate>
std::vector<Document> SearchServer::FindAllDocuments(std::execution::parallel_policy, const Query& query,
    DocumentPredicate document_predicate) const {
    size_t parts = static_cast<size_t>(word_to_document_freqs_.size());
    ConcurrentMap<int, double> document_to_relevance(parts);
    for_each(std::execution::par, query.minus_words.begin(), query.minus_words.end(), [this, &document_to_relevance](auto word) {
        if (word_to_document_freqs_.count(word) != 0) {
            for (const auto [document_id, _] : word_to_document_freqs_.at(word)) {
                document_to_relevance.Remove(document_id);
            }
        }
    });
    for_each(std::execution::par, query.plus_words.begin(), query.plus_words.end(), [this, &document_predicate, &document_to_relevance](auto word) {
        if (word_to_document_freqs_.count(word) != 0) {
            const double inverse_document_freq = SearchServer::ComputeWordInverseDocumentFreq(word);
            for (const auto [document_id, term_freq] : word_to_document_freqs_.at(word)) {
                const auto& document_data = documents_.at(document_id);
                if (document_predicate(document_id, document_data.status, document_data.rating)) {
                    document_to_relevance[document_id].ref_to_value += term_freq * inverse_document_freq;
                }
            }
        }
    });
    vector<Document> matched_documents;
    for (const auto [document_id, relevance] : document_to_relevance.BuildOrdinaryMap()) {
        matched_documents.push_back(
            { document_id, relevance, documents_.at(document_id).rating });
    }
    return matched_documents;
}