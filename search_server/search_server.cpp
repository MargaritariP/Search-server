#include "search_server.h"
#include <cmath>
#include <iostream>
#include <numeric>

using namespace std;

void SearchServer::AddDocument(int document_id, string_view document, DocumentStatus status,
    const vector<int>& ratings) {
    if (document_id < 0) {
        throw invalid_argument("Invalid document_id"s);
    }
    if ((documents_.count(document_id) > 0)) {
        throw invalid_argument("Existing document"s);
    }
    string text{ document.begin(), document.end() };
    documents_.emplace(document_id, DocumentData{ ComputeAverageRating(ratings), status, text });
    auto words = SplitIntoWordsNoStop(documents_.at(document_id).text);
    const double inv_word_count = 1.0 / words.size();
    for (auto word : words) {
        word_to_document_freqs_[word][document_id] += inv_word_count;
        doc_to_word_freq[document_id][word] += inv_word_count;
    }
    document_ids_.insert(document_id);
}
vector<Document> SearchServer::FindTopDocuments(string_view raw_query, DocumentStatus status) const {
    return FindTopDocuments(
        raw_query, [&status](int document_id, DocumentStatus document_status, int rating) {
            return document_status == status;
        });
}
vector<Document> SearchServer::FindTopDocuments(string_view raw_query) const {
    return FindTopDocuments(raw_query, DocumentStatus::ACTUAL);
}
int SearchServer::GetDocumentCount() const {
    return documents_.size();
}
set<int>::const_iterator SearchServer::begin() const {
    return document_ids_.begin();
}
set<int>::const_iterator SearchServer::end() const {
    return document_ids_.end();
}
const map<string_view, double>& SearchServer::GetWordFrequencies(int document_id) const {
    static const map<string_view, double> empty;
    if (doc_to_word_freq.count(document_id) == 0) {
        return empty;
    }
    else {
        return doc_to_word_freq.at(document_id);
    }
}
tuple<vector<string_view>, DocumentStatus> SearchServer::MatchDocument(string_view raw_query,
    int document_id) const {
    if ((document_id < 0) || (documents_.count(document_id) == 0)) {
        throw out_of_range("Document`s id does not exist"s);
    }
    const auto query = ParseQuery(raw_query, false);
    vector<string_view> matched_words;
    for (auto word : query.minus_words) {
        if (word_to_document_freqs_.count(word) == 0) {
            continue;
        }
        if (word_to_document_freqs_.at(word).count(document_id)) {
            return { {}, documents_.at(document_id).status };
        }
    }
    for (auto word : query.plus_words) {
        if (word_to_document_freqs_.count(word) == 0) {
            continue;
        }
        if (word_to_document_freqs_.at(word).count(document_id)) {
            matched_words.push_back(word);
        }
    }
    return { matched_words, documents_.at(document_id).status };
}
tuple<vector<string_view>, DocumentStatus> SearchServer::MatchDocument(execution::sequenced_policy, string_view raw_query,
    int document_id) const {
    return  MatchDocument(raw_query, document_id);
}
tuple<vector<string_view>, DocumentStatus> SearchServer::MatchDocument(execution::parallel_policy, string_view raw_query, int document_id) const {
    if ((document_id < 0) || (documents_.count(document_id) == 0)) {
        throw out_of_range("Document`s id does not exist"s);
    }
    const auto query = ParseQuery(raw_query, true);
    vector<string_view> matched_words(query.plus_words.size());
    if (std::any_of(execution::par, query.minus_words.begin(), query.minus_words.end(), [&](string_view word) {
        return word_to_document_freqs_.at(word).count(document_id);
        })) {
        return { {}, documents_.at(document_id).status };
    }
    auto last = copy_if(execution::par, query.plus_words.begin(), query.plus_words.end(), matched_words.begin(), [&](string_view word) {
        return word_to_document_freqs_.at(word).count(document_id);
        });
    matched_words.erase(last, matched_words.end());
    sort(execution::par, matched_words.begin(), matched_words.end());
    auto it = unique(matched_words.begin(), matched_words.end());
    matched_words.erase(it, matched_words.end());
    return { matched_words, documents_.at(document_id).status };
}
void SearchServer::RemoveDocument(int document_id) {
    if (!document_ids_.count(document_id)) {
        return;
    }
    auto search_doc = doc_to_word_freq.at(document_id);
    for (auto [str, x] : search_doc) {
        word_to_document_freqs_[str].erase(document_id);
    }
    doc_to_word_freq.erase(document_id);
    documents_.erase(document_id);
    document_ids_.erase(document_id);
}
void SearchServer::RemoveDocument(execution::sequenced_policy, int document_id) {
    return RemoveDocument(document_id);
}
void SearchServer::RemoveDocument(execution::parallel_policy, int document_id) {
    if (!document_ids_.count(document_id)) {
        return;
    }
    vector<string_view> words;
    for (const auto [str, x] : doc_to_word_freq.at(document_id)) {
        words.push_back(str);
    }
    for_each(execution::par, words.begin(), words.end(), 
        [this, &document_id](auto word) { word_to_document_freqs_.at(word).erase(document_id);
        });
    doc_to_word_freq.erase(document_id);
    documents_.erase(document_id);
    document_ids_.erase(document_id);
}
bool SearchServer::IsStopWord(string_view word) const {
    return stop_words_.count(word) > 0;
}
bool SearchServer::IsValidWord(string_view word) {
    return none_of(word.begin(), word.end(), [](char c) {
        return c >= '\0' && c < ' ';
        });
}
vector<string_view> SearchServer::SplitIntoWordsNoStop(string_view text) const {
    vector<string_view> words;
    for (auto word : SplitIntoWordsView(text)) {
        if (!IsValidWord(word)) {
            throw invalid_argument("Word is invalid"s);
        }
        if (!IsStopWord(word)) {
            words.push_back(word);
        }
    }
    return words;
}
int SearchServer::ComputeAverageRating(const vector<int>& ratings) {
    if (ratings.empty()) {
        return 0;
    }
    int rating_sum = accumulate(ratings.begin(), ratings.end(), 0);
    return rating_sum / static_cast<int>(ratings.size());
}
SearchServer::QueryWord SearchServer::ParseQueryWord(string_view text) const {
    if (text.empty()) {
        throw invalid_argument("Query word is empty"s);
    }
    auto word = text;
    bool is_minus = false;
    if (word[0] == '-') {
        is_minus = true;
        word = word.substr(1);
    }
    if (word.empty() || word[0] == '-' || !IsValidWord(word)) {
        throw invalid_argument("Query word is invalid");
    }
    return { word, is_minus, IsStopWord(word) };
}
SearchServer::Query SearchServer::ParseQuery(string_view text, bool is_sorted) const {
    SearchServer::Query query;
    for (auto word : SplitIntoWordsView(text)) {
        const auto query_word = ParseQueryWord(word);
        if (!query_word.is_stop) {
            if (query_word.is_minus) {
                query.minus_words.push_back(query_word.data);
            }
            else {
                query.plus_words.push_back(query_word.data);
            }
        }
    }
    if (!is_sorted) {
        sort(query.plus_words.begin(), query.plus_words.end());
        auto it_p = unique(query.plus_words.begin(), query.plus_words.end());
        query.plus_words.erase(it_p, query.plus_words.end());

        sort(query.minus_words.begin(), query.minus_words.end());
        auto it_m = unique(query.minus_words.begin(), query.minus_words.end());
        query.minus_words.erase(it_m, query.minus_words.end()); 
    }
    return query;
}
double SearchServer::ComputeWordInverseDocumentFreq(string_view word) const {
    return log(GetDocumentCount() * 1.0 / word_to_document_freqs_.at(word).size());
}