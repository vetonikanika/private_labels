#include <cassert>
#include <utility>

#include "seal/seal.h"

#include "hashing.h"
#include "polynomials.cpp"
#include "random.h"

#include "psi.h"

#define DEBUG

#ifdef DEBUG
// this code will only be compiled in debug mode.
#include <iostream>
// we save the receiver's key in a global variable, because it is helpful to
// have access to it when debugging sender code.
SecretKey *receiver_key_leaked;
#endif


PSIParams::PSIParams(size_t receiver_size, size_t sender_size, size_t input_bits)
    : receiver_size(receiver_size),
      sender_size(sender_size),
      input_bits(input_bits)
{
    EncryptionParameters parms(scheme_type::BFV);
    parms.set_poly_modulus_degree(8192 * 2);
    parms.set_coeff_modulus(DefaultParams::coeff_modulus_128(8192 * 2));
    // for batching to work, the plain modulus must be a prime that's equal
    // to 1 mod (2 * poly_modulus_degree).
    // TODO: choose this optimally (it should be a little over
    // 2^(input_bits - bucket_count_log() + 2)).
    parms.set_plain_modulus((8192 * 2 * 4) + 1);
    context = SEALContext::Create(parms);
}

size_t PSIParams::hash_functions() {
    return 3;
}

size_t PSIParams::bucket_count_log() {
    // we want to have a number of buckets that is a power of two and a little
    // bigger than receiver_size, so let's output ceil(log2(receiver_size)) + 1
    // (so we'll have between 2x and 4x buckets).
    size_t result = 0;
    while ((1ul << result) < receiver_size) {
        result++;
    }

    return result + 1;
}

size_t PSIParams::sender_bucket_capacity() {
    // TODO: fix this
    // see Table 1 in [CLR17]
    return 10;
}

uint64_t PSIParams::encode_bucket_element(bucket_slot &element, bool is_receiver) {
    if (element != BUCKET_EMPTY) {
        // we need to encode:
        // - the input itself, except for the last bucket_count_log() bits
        //   (thanks to permutation-based hashing)
        // - the index of the hash function used to hash it into its bucket.
        //   this should be in [0, 1, 2]; index 3 is used for dummy elements.
        assert(element.second < 3);
        return (((element.first >> bucket_count_log()) << 2)
                | (element.second));
    } else {
        // for the dummy element, we use a non-existent hash funcion index (3)
        // and 0 or 1 for the input depending on whether it's the sender or the]
        // receiver who needs a dummy.
        return 3 | ((is_receiver ? 1 : 0) << 2);
    }
}

void PSIParams::generate_seeds() {
    seeds.clear();
    auto random_factory = UniformRandomGeneratorFactory::default_factory();
    auto random = random_factory->create();

    for (size_t i = 0; i < hash_functions(); i++) {
        seeds.push_back(random_bits(random, 64));
    }
}

void PSIParams::set_seeds(vector<uint64_t> &seeds_ext) {
    assert(seeds_ext.size() == hash_functions());
    seeds = seeds_ext;
}

PSIReceiver::PSIReceiver(PSIParams &params)
    : params(params),
      keygen(params.context),
      public_key_(keygen.public_key()),
      secret_key(keygen.secret_key())
{
#ifdef DEBUG
    receiver_key_leaked = &secret_key;
#endif
}

vector<Ciphertext> PSIReceiver::encrypt_inputs(vector<uint64_t> &inputs)
{
    assert(inputs.size() == params.receiver_size);

    Encryptor encryptor(params.context, public_key_);
    BatchEncoder encoder(params.context);
    // confusing naming here: slot_count refers to slots in the batched
    // plaintexts, not in the hash table.
    size_t slot_count = encoder.slot_count();

    auto random_factory = UniformRandomGeneratorFactory::default_factory();
    auto random = random_factory->create();

    size_t bucket_count_log = params.bucket_count_log();
    size_t bucket_count = 1 << bucket_count_log;
    vector<bucket_slot> buckets(bucket_count, BUCKET_EMPTY);
    bool res = cuckoo_hash(random, inputs, bucket_count_log, buckets, params.seeds);
    assert(res); // TODO: handle gracefully

    // each ciphertext will encode (at most) slot_count buckets, so we'll
    // need ceil(m / slot_count) ciphertexts.
    size_t ciphertext_count = (bucket_count + (slot_count - 1)) / slot_count;
    vector<Ciphertext> result(ciphertext_count);

    Plaintext buckets_grouped(slot_count, slot_count);

    for (size_t i = 0; i < ciphertext_count; i++) {
        // figure out how many buckets we'll be putting into this ciphertext:
        // this is slot_count for all blocks except the last one
        size_t buckets_here = (i < ciphertext_count - 1)
                              ? slot_count
                              : (bucket_count % slot_count);

        buckets_grouped.resize(buckets_here);
        for (size_t j = 0; j < buckets_here; j++) {
            buckets_grouped[j] = params.encode_bucket_element(buckets[slot_count * i + j], true);
        }

        // encode all of the buckets, in-place
        encoder.encode(buckets_grouped);

        encryptor.encrypt(buckets_grouped, result[i]);
    }

    // after completing the protocol, the receiver will learn which locations
    // *in the hash table* matched. in order for that information to be useful,
    // they need to know where each element of their input vector went in the
    // hash table. to enable that, let's rearrange the input vector so that now
    // everything is where it was hashed to.
    // TODO: this is kind of a hack and needs to be better-documented or maybe
    // replaced.
    inputs.resize(bucket_count);
    for (size_t i = 0; i < bucket_count; i++) {
        inputs[i] = buckets[i].first;
    }

    return result;
}

vector<size_t> PSIReceiver::decrypt_matches(vector<Ciphertext> &encrypted_matches)
{
    Decryptor decryptor(params.context, secret_key);
    BatchEncoder encoder(params.context);
    size_t slot_count = encoder.slot_count();

    size_t bucket_count = (1 << params.bucket_count_log());

    vector<size_t> result;

    for (size_t i = 0; i < encrypted_matches.size(); i++) {
        Plaintext decrypted;
        decryptor.decrypt(encrypted_matches[i], decrypted);

        // decode in-place
        encoder.decode(decrypted);

        for (size_t j = 0; (j < slot_count) && (slot_count * i + j < bucket_count); j++) {
            if (decrypted[j] == 0) {
                result.push_back(slot_count * i + j);
            }
        }
    }

    return result;
}

PublicKey& PSIReceiver::public_key()
{
    return public_key_;
}

RelinKeys PSIReceiver::relin_keys()
{
    return keygen.relin_keys(8);
}

PSISender::PSISender(PSIParams &params)
    : params(params)
{}

vector<Ciphertext> PSISender::compute_matches(vector<uint64_t> &inputs,
                                              PublicKey& receiver_public_key,
                                              RelinKeys relin_keys,
                                              vector<Ciphertext> &receiver_inputs)
{
    assert(inputs.size() == params.sender_size);

    vector<Ciphertext> result(receiver_inputs.size());

    auto random_factory = UniformRandomGeneratorFactory::default_factory();
    auto random = random_factory->create();

    uint64_t plain_modulus = params.context->context_data()->parms().plain_modulus().value();

    size_t bucket_count_log = params.bucket_count_log();
    size_t bucket_count = (1 << bucket_count_log);
    size_t capacity = params.sender_bucket_capacity();
    vector<bucket_slot> buckets(bucket_count * capacity, BUCKET_EMPTY);
    bool res = complete_hash(random, inputs, bucket_count_log, capacity, buckets, params.seeds);
    assert(res); // TODO: handle gracefully

    Encryptor encryptor(params.context, receiver_public_key);
    BatchEncoder encoder(params.context);
    size_t slot_count = encoder.slot_count();

    Evaluator evaluator(params.context);

    // for each bucket, compute the coefficients of the polynomial
    // f(x) = \prod_{y in bucket} (x - y)
    vector<vector<uint64_t>> f_coeffs(bucket_count);
    vector<uint64_t> this_bucket(capacity);
    for (size_t i = 0; i < bucket_count; i++) {
        // TODO: rewrite polynomial_from_roots to take iterators to make this
        // unnecessary
        for (size_t j = 0; j < capacity; j++) {
            this_bucket[j] = params.encode_bucket_element(buckets[i * capacity + j], false);
        }

        f_coeffs[i] = polynomial_from_roots(this_bucket, plain_modulus);
    }

    // now, for each 0 <= j <= capacity, encode a bunch of vectors
    // corresponding to the jth coefficients of the corresponding polynomials
    // ---one for each group of buckets batched into one ciphertext.
    size_t ciphertext_count = (bucket_count + (slot_count - 1)) / slot_count;
    assert(ciphertext_count == receiver_inputs.size());

    Plaintext coeffs_grouped(slot_count, slot_count);
    vector<vector<Plaintext>> f_coeffs_enc(ciphertext_count);
    for (size_t i = 0; i < ciphertext_count; i++) {
        // figure out how many coefficients we'll be putting into this
        // vector: this is slot_count for all blocks except the last one
        size_t coeffs_here = (i < ciphertext_count - 1)
                             ? slot_count
                             : (bucket_count % slot_count);

        coeffs_grouped.resize(coeffs_here);
        for (size_t j = 0; j < capacity + 1; j++) {
            for (size_t k = 0; k < coeffs_here; k++) {
                coeffs_grouped[k] = f_coeffs[slot_count * i + k][j];
            }

            encoder.encode(coeffs_grouped);
            f_coeffs_enc[i].push_back(coeffs_grouped);
        }
    }

    // encrypt the constant terms of the polynomials and put them in result[i],
    // so that the other terms can be added to them.
    for (size_t i = 0; i < ciphertext_count; i++) {
        encryptor.encrypt(f_coeffs_enc[i][0], result[i]);
    }

    vector<Ciphertext> powers(capacity + 1);

    for (size_t i = 0; i < receiver_inputs.size(); i++) {
        // first, compute all the powers of the receiver's input
        // NB: powers[0] is undefined!
        powers[1] = receiver_inputs[i];
        for (size_t j = 2; j < powers.size(); j++) {
            if (j & 2 == 0) {
                evaluator.square(powers[j >> 1], powers[j]);
            } else {
                evaluator.multiply(powers[j - 1], powers[1], powers[j]);
            }
            evaluator.relinearize_inplace(powers[j], relin_keys);
        }

        // now use the computed powers to evaluate f(input)
        // recall that the const terms of the polynomials are already in
        // result[i].

#ifdef DEBUG
        Decryptor decryptor(params.context, *receiver_key_leaked);
        cerr << "computing matches for receiver batch #" << i << endl;
        cerr << "initially the noise budget is " << decryptor.invariant_noise_budget(result[i]) << endl;
#endif

        for (size_t j = 1; j < f_coeffs_enc[i].size(); j++) {
            // term = receiver_inputs[i]^j * f_coeffs[i][j]
            Ciphertext term;
            // multiply_plain does not allow the second parameter to be zero.
            if (!f_coeffs_enc[i][j].is_zero()) {
                evaluator.multiply_plain(powers[j], f_coeffs_enc[i][j], term);
                evaluator.relinearize_inplace(term, relin_keys);
                evaluator.add_inplace(result[i], term);
            }

#ifdef DEBUG
        cerr << "after term " << j << " it is " << decryptor.invariant_noise_budget(result[i]) << endl;
#endif
        }

        // now multiply the result of each computation by a random mask
        Plaintext random_mask(slot_count, slot_count);
        for (size_t j = 0; j < slot_count; j++) {
            random_mask[j] = random_nonzero_integer(random, plain_modulus);
        }
        encoder.encode(random_mask);
        evaluator.multiply_plain_inplace(result[i], random_mask);
        // since we're done computing on this, this relinearization is really
        // only helpful to decrease communication costs
        evaluator.relinearize_inplace(result[i], relin_keys);
    }

    return result;
}
