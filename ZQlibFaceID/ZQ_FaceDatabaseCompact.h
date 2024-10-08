#ifndef _ZQ_FACE_DATABASE_COMPACT_H_
#define _ZQ_FACE_DATABASE_COMPACT_H_
#pragma once

#include <malloc.h>
#include <stdio.h>
#include <vector>
#include <omp.h>
#include "ZQ_FaceRecognizerSphereFace.h"
#include "ZQ_MathBase.h"
#include "ZQ_MergeSort.h"

namespace ZQ
{
	class ZQ_FaceDatabaseCompact
	{
		enum CONST_VAL {
			FEAT_ALIGNED_SIZE = 32
		};
	public:
		ZQ_FaceDatabaseCompact() 
		{
			dim = 0;
			person_num = 0;
			person_face_num = 0;
			total_face_num = 0;
			person_face_offset = 0;
			all_face_feats = 0;
		}
		~ZQ_FaceDatabaseCompact() {}

		bool LoadFromFile(const char* feats_file, const char* names_file)
		{
			_clear();
			if (!_load_feats(feats_file))
			{
				_clear();
				return false;
			}
			if (!_load_names(names_file))
			{
				_clear();
				return false;
			}
			if (person_num != names.size())
			{
				_clear();
				return false;
			}
			return true;
		}

		bool GenerateRandomDatabase(int num_person, int num_feat_per_person, int dim)
		{
			if (dim != 128 && dim != 256 && dim != 512)
			{
				printf("dim must be 128 or 256 or 512\n");
				return false;
			}
			_clear();
			
			std::vector<std::string> tmp_names;
			for (__int64 i = 0; i < num_person; i++)
			{
				char buf[200];
				sprintf(buf, "%d", i);
				tmp_names.push_back(std::string(buf));
			}
			int* tmp_person_face_num = (int*)malloc(sizeof(int)*num_person);
			for (int i = 0; i < num_person; i++)
				tmp_person_face_num[i] = num_feat_per_person;

			__int64* tmp_person_face_offset = (__int64*)malloc(sizeof(__int64)*num_person);
			for (__int64 i = 0; i < num_person; i++)
			{
				tmp_person_face_offset[i] = i * num_feat_per_person;
			}

			__int64 num_all_feats = num_person * num_feat_per_person;
			__int64 needed_bytes = num_all_feats * dim * sizeof(float);
			float* tmp_all_feats = (float*)_aligned_malloc(needed_bytes, FEAT_ALIGNED_SIZE);
			
			printf("need %d MB \n", needed_bytes / 1024 / 1024);
			if (tmp_all_feats == 0)
			{
				printf("failed to alloc memory, need %ld bytes\n", needed_bytes);
				return false;
			}
			for (__int64 i = 0; i < num_all_feats*dim; i++)
			{
				tmp_all_feats[i] = rand() % 1001 / 1000.0;
			}
			for (__int64 i = 0; i < num_all_feats; i++)
			{
				ZQ_MathBase::Normalize(dim, tmp_all_feats + i*dim);
			}
			printf("done\n");
			
			this->dim = dim;
			this->person_num = num_person;
			this->person_face_num = tmp_person_face_num;
			this->total_face_num = num_all_feats;
			this->person_face_offset = tmp_person_face_offset;
			this->all_face_feats = tmp_all_feats;
			this->names.swap(tmp_names);

			return true;
		}

		bool Search(int feat_dim, int feat_num, const float* feat, std::vector<int>& out_ids,
			std::vector<float>& out_scores, std::vector<std::string>& out_names, int max_num, int max_thread_num) const
		{
			return _find_the_best_matches(feat_dim, feat_num, feat, out_ids, out_scores, out_names, max_num, max_thread_num);
		}

		bool ExportSimilarityForAllPairs(const std::string& out_score_file, const std::string& out_flag_file, 
			__int64& all_pair_num, __int64& same_pair_num, __int64& notsame_pair_num, int max_thread_num, bool quantization) const
		{
			return _export_similarity_for_all_pairs(out_score_file,out_flag_file, all_pair_num,
				same_pair_num, notsame_pair_num, max_thread_num, quantization);
		}

		bool DetectRepeatPerson(const std::string& out_file, int max_thread_num, float similarity_thresh = 0.5, bool only_pivot = true) const
		{
			return _detect_repeat_person(out_file, max_thread_num, similarity_thresh, only_pivot);
		}

	private:
		int dim;
		int person_num;
		int* person_face_num;
		__int64 total_face_num;
		__int64* person_face_offset;
		float* all_face_feats;
		std::vector<std::string> names;

	private:
		void _clear()
		{
			dim = 0;
			person_num = 0;
			total_face_num = 0;
			if (person_face_num)
			{
				free(person_face_num);
				person_face_num = 0;
			}
			if (person_face_offset)
			{
				free(person_face_offset);
				person_face_offset = 0;
			}
			if (all_face_feats)
			{
				_aligned_free(all_face_feats);
				all_face_feats = 0;
			}
			names.clear();
		}

		bool _load_feats(const char* file)
		{
			FILE* in = 0;
#if defined(_WIN32)
			if (0 != fopen_s(&in, file, "rb"))
			{
				return false;
			}
#else
			in = fopen(file, "rb");
			if (in == NULL)
				return false;
#endif

			if (1 != fread(&dim, sizeof(int), 1, in) || dim <= 0)
			{
				fclose(in);
				return false;
			}

			if (1 != fread(&person_num, sizeof(int), 1, in) || person_num <= 0)
			{
				fclose(in);
				return false;
			}

			person_face_num = (int*)malloc(sizeof(int)*person_num);
			person_face_offset = (__int64*)malloc(sizeof(__int64)*person_num);
			if (person_face_num == 0 || person_face_offset == 0)
			{
				fclose(in);
				return false;
			}
			if (person_num != fread(person_face_num, sizeof(int), person_num, in))
			{
				fclose(in);
				return false;
			}
			total_face_num = 0;
			for (int i = 0; i < person_num; i++)
			{
				if (person_face_num[i] <= 0)
				{
					fclose(in);
					return false;
				}
				person_face_offset[i] = total_face_num;
				total_face_num += person_face_num[i];
			}

			all_face_feats = (float*)_aligned_malloc(sizeof(float)*total_face_num*dim, FEAT_ALIGNED_SIZE);
			if (all_face_feats == 0)
			{
				fclose(in);
				return false;
			}

			if (total_face_num*dim != fread(all_face_feats, sizeof(float), total_face_num*dim, in))
			{
				fclose(in);
				return false;
			}
			fclose(in);
			return true;
		}

		bool _load_names(const char* file)
		{
			FILE* in = 0;
#if defined(_WIN32)
			if (0 != fopen_s(&in, file, "r"))
				return false;
#else
			in = fopen(file, "r");
			if (in == NULL)
				return false;
#endif
			char line[200] = { 0 };
			while (true)
			{
				line[0] = '\0';
				fgets(line, 199, in);
				if (line[0] == '\0')
					break;
				int len = strlen(line);
				if (line[len - 1] == '\n')
					line[--len] = '\0';
				names.push_back(std::string(line));
			}

			fclose(in);
			return true;
		}

		bool _find_the_best_matches(int feat_dim, int feat_num, const float* feat, std::vector<int>& out_ids,
			std::vector<float>& out_scores, std::vector<std::string>& out_names, int max_num, int max_thread_num) const 
		{
			if (person_num <= 0 || feat_dim != dim || feat_num <= 0)
				return false;

			__int64 widthStep = (sizeof(float)*dim + FEAT_ALIGNED_SIZE-1) / FEAT_ALIGNED_SIZE * FEAT_ALIGNED_SIZE;

			float* feat_aligned = (float*)_aligned_malloc(widthStep*feat_num, FEAT_ALIGNED_SIZE);
			if (feat_aligned == 0)
				return false;
			for(__int64 i = 0;i < feat_num;i++)
				memcpy(((char*)feat_aligned)+widthStep*i, feat+feat_dim*i, sizeof(float)*dim);
			float* scores = (float*)malloc(sizeof(float)*total_face_num);
			if (scores == 0)
			{
				_aligned_free(feat_aligned);
				return false;
			}
			for (__int64 i = 0; i < total_face_num; i++)
				scores[i] = -FLT_MAX;

			int num_procs = omp_get_num_procs();
			int real_threads = __max(1, __min(max_thread_num, num_procs - 1));

			if (real_threads == 1)
			{
				for (__int64 j = 0; j < feat_num; j++)
				{
					float* tmp_feat = (float*)(((char*)feat_aligned) + widthStep*j);
					__int64 chunk_size = (total_face_num + real_threads - 1) / real_threads;
					if (dim == 128)
					{
						for (__int64 i = 0; i < total_face_num; i++)
						{
							scores[i] = __max(scores[i],ZQ_FaceRecognizerSphereFace::_cal_similarity_avx_dim128(tmp_feat, all_face_feats + i*dim));
						}
					}
					else if (dim == 256)
					{
						for (__int64 i = 0; i < total_face_num; i++)
						{
							scores[i] = __max(scores[i], ZQ_FaceRecognizerSphereFace::_cal_similarity_avx_dim256(tmp_feat, all_face_feats + i*dim));
						}
					}
					else if (dim == 512)
					{
						for (__int64 i = 0; i < total_face_num; i++)
						{
							scores[i] = __max(scores[i], ZQ_FaceRecognizerSphereFace::_cal_similarity_avx_dim512(tmp_feat, all_face_feats + i*dim));
						}
					}
					else
					{
						for (__int64 i = 0; i < total_face_num; i++)
						{
							scores[i] = __max(scores[i], ZQ_MathBase::DotProduct(dim, tmp_feat, all_face_feats + i*dim));
						}
					}
				}
			}
			else
			{
				for (__int64 j = 0; j < feat_num; j++)
				{
					float* tmp_feat = (float*)(((char*)feat_aligned) + widthStep*j);
					__int64 chunk_size = (total_face_num + real_threads - 1) / real_threads;
					if (dim == 128)
					{
#pragma omp parallel for schedule(static, chunk_size) num_threads(real_threads)
						for (__int64 i = 0; i < total_face_num; i++)
						{
							scores[i] = __max(scores[i], ZQ_FaceRecognizerSphereFace::_cal_similarity_avx_dim128(tmp_feat, all_face_feats + i*dim));
						}
					}
					else if (dim == 256)
					{
#pragma omp parallel for schedule(static, chunk_size) num_threads(real_threads)
						for (__int64 i = 0; i < total_face_num; i++)
						{
							scores[i] = __max(scores[i], ZQ_FaceRecognizerSphereFace::_cal_similarity_avx_dim256(tmp_feat, all_face_feats + i*dim));
						}
					}
					else if (dim == 512)
					{
#pragma omp parallel for schedule(static, chunk_size) num_threads(real_threads)
						for (__int64 i = 0; i < total_face_num; i++)
						{
							scores[i] = __max(scores[i], ZQ_FaceRecognizerSphereFace::_cal_similarity_avx_dim512(tmp_feat, all_face_feats + i*dim));
						}
					}

					else
					{
#pragma omp parallel for schedule(static, chunk_size) num_threads(real_threads)
						for (__int64 i = 0; i < total_face_num; i++)
						{
							scores[i] = __max(scores[i], ZQ_MathBase::DotProduct(dim, tmp_feat, all_face_feats + i*dim));
						}
					}
				}
			}
			

			float* max_scores = (float*)malloc(sizeof(float)*person_num);
			if (max_scores == 0)
			{
				_aligned_free(feat_aligned);
				free(scores);
				return false;
			}

			if (real_threads == 1)
			{
				for (__int64 i = 0; i < person_num; i++)
				{
					float tmp = -FLT_MAX;
					for (__int64 j = person_face_offset[i]; j < person_face_offset[i]+person_face_num[i]; j++)
					{
						tmp = __max(tmp, scores[j]);
					}
					max_scores[i] = tmp;
				}
			}
			else
			{
				int chunk_size = (person_num + real_threads - 1) / real_threads;
#pragma omp parallel for schedule(static, chunk_size) num_threads(real_threads)
				for (__int64 i = 0; i < person_num; i++)
				{
					float tmp = -FLT_MAX;
					for (__int64 j = person_face_offset[i]; j < person_face_offset[i] + person_face_num[i]; j++)
					{
						tmp = __max(tmp, scores[j]);
					}
					max_scores[i] = tmp;
				}
			}
			
			_aligned_free(feat_aligned);
			free(scores);

			
			int* ids = (int*)malloc(sizeof(int)*person_num);
			if (ids == 0)
			{
				free(max_scores);
				return false;
			}
			for (__int64 i = 0; i < person_num; i++)
			{
				ids[i] = i;
			}


			out_ids.clear();
			out_scores.clear();
			out_names.clear();
			for (__int64 i = 0; i < __min(max_num, person_num); i++)
			{
				float cur_max_score = max_scores[i];
				int max_id = i;
				for (__int64 j = i + 1; j < person_num; j++)
				{
					if (cur_max_score < max_scores[j])
					{
						max_id = j;
						cur_max_score = max_scores[j];
					}
				}
				int tmp_id = ids[i];
				ids[i] = ids[max_id];
				ids[max_id] = tmp_id;
				float tmp_score = max_scores[i];
				max_scores[i] = max_scores[max_id];
				max_scores[max_id] = tmp_score;

				out_ids.push_back(ids[i]);
				out_scores.push_back(max_scores[i]);
				out_names.push_back(names[ids[i]]);
			}
		
			free(max_scores);
			free(ids);
			return true;
		}

		//must be aligned
		static float _compute_similarity(int dim, const float* v1, const float* v2)
		{
			if (dim == 128)
				return ZQ_FaceRecognizerSphereFace::_cal_similarity_avx_dim128(v1, v2);
			else if (dim == 256)
				return ZQ_FaceRecognizerSphereFace::_cal_similarity_avx_dim256(v1, v2);
			else if (dim == 512)
				return ZQ_FaceRecognizerSphereFace::_cal_similarity_avx_dim512(v1, v2);
			else
				return ZQ_MathBase::DotProduct(dim, v1, v2);
		}

		bool _export_similarity_for_all_pairs(const std::string& out_score_file, const std::string& out_flag_file, 
			__int64& all_pair_num, __int64& same_pair_num, __int64& notsame_pair_num, int max_thread_num, bool quantization) const
		{
			FILE* out1 = 0;
#if defined(_WIN32)
			if (0 != fopen_s(&out1, out_score_file.c_str(), "wb"))
			{
				printf("failed to create file %s\n", out_score_file.c_str());
				return false;
			}
#else
			out1 = fopen(out_score_file.c_str(), "wb");
			if (out1 == NULL)
			{
				printf("failed to create file %s\n", out_score_file.c_str());
				return false;
			}
#endif

			FILE* out2 = 0;
#if defined(_WIN32)
			if (0 != fopen_s(&out2, out_flag_file.c_str(), "wb"))
			{
				printf("failed to create file %s\n", out_flag_file.c_str());
				fclose(out1);
				return false;
			}
#else
			out2 = fopen(out_flag_file.c_str(), "wb");
			if (out2 == NULL)
			{
				printf("failed to create file %s\n", out_flag_file.c_str());
				fclose(out1);
				return false;
			}
#endif
			
			all_pair_num = total_face_num *(total_face_num - 1) / 2;
			same_pair_num = 0;
			notsame_pair_num = 0;
			//fprintf(out, "%lld\n", total_pair_num);
			int real_thread_num = __max(1, __min(max_thread_num, omp_get_num_procs() - 1));
			if (real_thread_num == 1)
			{
				for (int pp = 0; pp < person_num; pp++)
				{
					__int64 cur_face_offset = person_face_offset[pp];
					__int64 cur_face_num = person_face_num[pp];
					__int64 max_pair_num = (total_face_num - cur_face_offset - 1);
					std::vector<float> scores(max_pair_num);
					std::vector<char> flags(max_pair_num);
					for (__int64 i = 0; i < cur_face_num; i++)
					{
						float* cur_i_feat = all_face_feats + (cur_face_offset + i)*dim;
						float* cur_j_feat;
						int idx = 0;
						for (__int64 j = i + 1; j < cur_face_num; j++)
						{
							cur_j_feat = all_face_feats + (cur_face_offset + j)*dim;
							scores[idx] = _compute_similarity(dim, cur_i_feat, cur_j_feat);
							flags[idx] = 1;
							same_pair_num++;
							idx++;
						}
						if (pp + 1 < person_num)
						{
							for (__int64 j = person_face_offset[pp + 1]; j < total_face_num; j++)
							{
								cur_j_feat = all_face_feats + j*dim;
								scores[idx] = _compute_similarity(dim, cur_i_feat, cur_j_feat);
								flags[idx] = 0;
								notsame_pair_num++;
								idx++;
							}
						}
						if (idx > 0)
						{
							if (quantization)
							{
								std::vector<short> short_scores(idx);
								for (int j = 0; j < idx; j++)
									short_scores[j] = __min(SHRT_MAX, __max(-SHRT_MAX, scores[j] * SHRT_MAX));
								fwrite(&short_scores[0], sizeof(short), idx, out1);
							}
							else
							{
								fwrite(&scores[0], sizeof(float), idx, out1);
							}
							fwrite(&flags[0], 1, idx, out2);
						}
					}
					printf("%d/%d handled\n", pp + 1, person_num);
				}
			}
			else
			{
				int chunk_size = 100;
				int handled[1] = { 0 };
				__int64 tmp_same_pair_num[1] = { 0 };
				printf("real_thread_num = %d\n", real_thread_num);
#pragma omp parallel for schedule(dynamic,chunk_size) num_threads(real_thread_num) shared(handled)
				for (int pp = 0; pp < person_num; pp++)
				{

					__int64 cur_face_offset = person_face_offset[pp];
					__int64 cur_face_num = person_face_num[pp];
					__int64 max_pair_num = (total_face_num - cur_face_offset-1);
					std::vector<float> scores(max_pair_num);
					std::vector<char> flags(max_pair_num);
					for (__int64 i = 0; i < cur_face_num; i++)
					{
						float* cur_i_feat = all_face_feats + (cur_face_offset + i)*dim;
						float* cur_j_feat;
						int idx = 0;
						for (__int64 j = i+1; j < cur_face_num; j++)
						{
							cur_j_feat = all_face_feats + (cur_face_offset + j)*dim;
							scores[idx] = _compute_similarity(dim, cur_i_feat, cur_j_feat);
							flags[idx] = 1;
							idx++;
						}
						if (pp + 1 < person_num)
						{
							for (__int64 j = person_face_offset[pp + 1]; j < total_face_num; j++)
							{
								cur_j_feat = all_face_feats + j*dim;
								scores[idx] = _compute_similarity(dim, cur_i_feat, cur_j_feat);
								flags[idx] = 0;
								idx++;
							}
						}
#pragma omp critical
						{
							if (idx > 0)
							{
								for (int kk = 0; kk < idx; kk++)
								{
									(*tmp_same_pair_num) += flags[kk];
								}
								if (quantization)
								{
									std::vector<short> short_scores(idx);
									for (int j = 0; j < idx; j++)
										short_scores[j] = __min(SHRT_MAX, __max(-SHRT_MAX, scores[j] * SHRT_MAX));
									fwrite(&short_scores[0], sizeof(short), idx, out1);
								}
								else
								{
									fwrite(&scores[0], sizeof(float), idx, out1);
								}
								fwrite(&flags[0], 1, idx, out2);
							}
						}
					}
#pragma omp critical
					{
						(*handled) ++;
						printf("%d/%d\n", *handled, person_num);
					}
				}
				same_pair_num = tmp_same_pair_num[0];
				notsame_pair_num = all_pair_num - same_pair_num;
			}

			fclose(out1);
			fclose(out2);
			return true;
		}

		bool _detect_repeat_person(const std::string& out_file, int max_thread_num, float similarity_thresh, bool only_pivot) const
		{
			std::vector<std::pair<int, int> > repeat_pairs;
			std::vector<float> scores;
			if (!_detect_repeat_person(repeat_pairs, scores, max_thread_num, similarity_thresh, only_pivot))
			{
				return false;
			}

			__int64 num = scores.size();
			printf("num = %lld\n", num);
			if (num > 0)
			{
				ZQ_MergeSort::MergeSortWithData(&scores[0], &repeat_pairs[0], sizeof(std::pair<int, int>), num, false);
			}

			FILE* out = 0;
#if defined(_WIN32)
			if (0 != fopen_s(&out, out_file.c_str(), "w"))
			{
				return false;
			}
#else
			out = fopen(out_file.c_str(), "w");
			if (out == NULL)
			{
				return false;
			}
#endif
			for (__int64 i = 0; i < num; i++)
			{
				fprintf(out, "%.3f %s %s\n", scores[i], names[repeat_pairs[i].first].c_str(), names[repeat_pairs[i].second].c_str());
			}
			fclose(out);
			return true;
		}

		bool _detect_repeat_person(std::vector<std::pair<int, int> >& repeat_pairs, std::vector<float>& repeat_scores,
			int max_thread_num, float similarity_thresh, bool only_pivot) const
		{
			repeat_pairs.clear();
			repeat_scores.clear();

			if (only_pivot)
			{
				std::vector<int> pivot_ids(person_num);

				if (max_thread_num <= 1)
				{
					for (int p = 0; p < person_num; p++)
					{
						__int64 cur_offset = person_face_offset[p];
						__int64 cur_num = person_face_num[p];
						std::vector<float> scores(cur_num*cur_num);
						int idx = 0;
						for (__int64 i = 0; i < cur_num; i++)
						{
							float* cur_i_feat = all_face_feats + (cur_offset + i)*dim;
							float* cur_j_feat;
							scores[i*cur_num + i] = 1;
							for (__int64 j = i + 1; j < cur_num; j++)
							{
								cur_j_feat = all_face_feats + (cur_offset + j)*dim;
								float tmp_score = _compute_similarity(dim, cur_i_feat, cur_j_feat);
								scores[i*cur_num + j] = tmp_score;
								scores[j*cur_num + i] = tmp_score;
							}
						}

						int pivot_id = -1;
						float sum_score = -FLT_MAX;
						for (int i = 0; i < cur_num; i++)
						{
							float tmp_sum = 0;
							for (int j = 0; j < cur_num; j++)
								tmp_sum += scores[i*cur_num + j];
							if (sum_score < tmp_sum)
							{
								pivot_id = i;
								sum_score = tmp_sum;
							}
						}
						pivot_ids[p] = pivot_id;
					}

					//
					for (int i = 0; i < person_num; i++)
					{
						for (int j = i + 1; j < person_num; j++)
						{
							const float* cur_i_feat = all_face_feats + (person_face_offset[i] + pivot_ids[i])*dim;
							const float* cur_j_feat = all_face_feats + (person_face_offset[j] + pivot_ids[j])*dim;
							float tmp_score = _compute_similarity(dim, cur_i_feat, cur_j_feat);
							if (tmp_score >= similarity_thresh)
							{
								repeat_pairs.push_back(std::make_pair(i, j));
								repeat_scores.push_back(tmp_score);
							}
						}
					}
				}
				else
				{
					int chunk_size = (person_num + max_thread_num - 1) / max_thread_num;
#pragma omp parallel for schedule(static,chunk_size) num_threads(max_thread_num)
					for (int p = 0; p < person_num; p++)
					{
						__int64 cur_offset = person_face_offset[p];
						__int64 cur_num = person_face_num[p];
						std::vector<float> scores(cur_num*cur_num);
						int idx = 0;
						for (__int64 i = 0; i < cur_num; i++)
						{
							float* cur_i_feat = all_face_feats + (cur_offset + i)*dim;
							float* cur_j_feat;
							scores[i*cur_num + i] = 1;
							for (__int64 j = i + 1; j < cur_num; j++)
							{
								cur_j_feat = all_face_feats + (cur_offset + j)*dim;
								float tmp_score = _compute_similarity(dim, cur_i_feat, cur_j_feat);
								scores[i*cur_num + j] = tmp_score;
								scores[j*cur_num + i] = tmp_score;
							}
						}

						int pivot_id = -1;
						float sum_score = -FLT_MAX;
						for (int i = 0; i < cur_num; i++)
						{
							float tmp_sum = 0;
							for (int j = 0; j < cur_num; j++)
								tmp_sum += scores[i*cur_num + j];
							if (sum_score < tmp_sum)
							{
								pivot_id = i;
								sum_score = tmp_sum;
							}
						}
						pivot_ids[p] = pivot_id;
					}

#pragma omp parallel for schedule(static,chunk_size) num_threads(max_thread_num)
					for (int i = 0; i < person_num; i++)
					{
						for (int j = i + 1; j < person_num; j++)
						{
							const float* cur_i_feat = all_face_feats + (person_face_offset[i] + pivot_ids[i])*dim;
							const float* cur_j_feat = all_face_feats + (person_face_offset[j] + pivot_ids[j])*dim;
							float tmp_score = _compute_similarity(dim, cur_i_feat, cur_j_feat);
							if (tmp_score >= similarity_thresh)
							{
#pragma omp critical
								{
									repeat_pairs.push_back(std::make_pair(i, j));
									repeat_scores.push_back(tmp_score);
								}
							}
						}
					}
				}
			}
			else
			{
				if (max_thread_num <= 1)
				{
					int handled[1] = { 0 };
					for (int i = 0; i < person_num; i++)
					{
						for (int j = i + 1; j < person_num; j++)
						{
							float max_score = -FLT_MAX;
							for (int s = 0; s < person_face_num[i]; s++)
							{
								for (int t = 0; t < person_face_num[j]; t++)
								{
									const float* cur_i_feat = all_face_feats + (person_face_offset[i] + s)*dim;
									const float* cur_j_feat = all_face_feats + (person_face_offset[j] + t)*dim;
									float tmp_score = _compute_similarity(dim, cur_i_feat, cur_j_feat);
									max_score = __max(max_score, tmp_score);
								}
							}
							if (max_score >= similarity_thresh)
							{
								repeat_pairs.push_back(std::make_pair(i, j));
								repeat_scores.push_back(max_score);
							}
						}
						handled[0]++;
						if (handled[0] % 10 == 0)
						{
							printf("%d/%d handled\n", handled[0], person_num);
						}
					}
				}
				else
				{
					int handled[1] = { 0 };
					int chunk_size = (person_num + max_thread_num - 1) / max_thread_num;
#pragma omp parallel for schedule(static,chunk_size) num_threads(max_thread_num)
					for (int i = 0; i < person_num; i++)
					{
						for (int j = i + 1; j < person_num; j++)
						{
							float max_score = -FLT_MAX;
							for (int s = 0; s < person_face_num[i]; s++)
							{
								for (int t = 0; t < person_face_num[j]; t++)
								{
									const float* cur_i_feat = all_face_feats + (person_face_offset[i] + s)*dim;
									const float* cur_j_feat = all_face_feats + (person_face_offset[j] + t)*dim;
									float tmp_score = _compute_similarity(dim, cur_i_feat, cur_j_feat);
									max_score = __max(max_score, tmp_score);
								}
							}
							if (max_score >= similarity_thresh)
							{
#pragma omp critical
								{
									repeat_pairs.push_back(std::make_pair(i, j));
									repeat_scores.push_back(max_score);
								}
							}
						}
#pragma omp critical
						{
							handled[0] ++;
							if (handled[0] % 10 == 0)
							{
								printf("%d/%d handled\n", handled[0], person_num);
							}
						}
					}
				}
			}
			return true;
		}
	};
}
#endif
